/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "SensorService.h"

#include <aidl/android/hardware/sensors/ISensors.h>
#include <android-base/strings.h>
#include <android/content/pm/IPackageManagerNative.h>
#include <android/util/ProtoOutputStream.h>
#include <binder/ActivityManager.h>
#include <binder/BinderService.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include <binder/PermissionController.h>
#include <com_android_frameworks_sensorservice_flags.h>
#include <cutils/ashmem.h>
#include <cutils/misc.h>
#include <cutils/properties.h>
#include <frameworks/base/core/proto/android/service/sensor_service.proto.h>
#include <hardware/sensors.h>
#include <hardware_legacy/power.h>
#include <inttypes.h>
#include <log/log.h>
#include <math.h>
#include <openssl/digest.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <private/android_filesystem_config.h>
#include <sched.h>
#include <sensor/SensorEventQueue.h>
#include <sensorprivacy/SensorPrivacyManager.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/SystemClock.h>

#include <condition_variable>
#include <ctime>
#include <future>
#include <mutex>
#include <string>

#include "BatteryService.h"
#include "CorrectedGyroSensor.h"
#include "GravitySensor.h"
#include "LimitedAxesImuSensor.h"
#include "LinearAccelerationSensor.h"
#include "OrientationSensor.h"
#include "RotationVectorSensor.h"
#include "SensorDirectConnection.h"
#include "SensorEventAckReceiver.h"
#include "SensorEventConnection.h"
#include "SensorFusion.h"
#include "SensorInterface.h"
#include "SensorRecord.h"
#include "SensorRegistrationInfo.h"
#include "SensorServiceUtils.h"

using namespace std::chrono_literals;
namespace sensorservice_flags = com::android::frameworks::sensorservice::flags;

namespace android {
// ---------------------------------------------------------------------------

/*
 * Notes:
 *
 * - what about a gyro-corrected magnetic-field sensor?
 * - run mag sensor from time to time to force calibration
 * - gravity sensor length is wrong (=> drift in linear-acc sensor)
 *
 */

const char* SensorService::WAKE_LOCK_NAME = "SensorService_wakelock";
uint8_t SensorService::sHmacGlobalKey[128] = {};
bool SensorService::sHmacGlobalKeyIsValid = false;
std::map<String16, int> SensorService::sPackageTargetVersion;
Mutex SensorService::sPackageTargetVersionLock;
String16 SensorService::sSensorInterfaceDescriptorPrefix =
    String16("android.frameworks.sensorservice");
AppOpsManager SensorService::sAppOpsManager;
std::atomic_uint64_t SensorService::curProxCallbackSeq(0);
std::atomic_uint64_t SensorService::completedCallbackSeq(0);

#define SENSOR_SERVICE_DIR "/data/system/sensor_service"
#define SENSOR_SERVICE_HMAC_KEY_FILE  SENSOR_SERVICE_DIR "/hmac_key"
#define SENSOR_SERVICE_SCHED_FIFO_PRIORITY 10

// Permissions.
static const String16 sAccessHighSensorSamplingRatePermission(
        "android.permission.HIGH_SAMPLING_RATE_SENSORS");
static const String16 sDumpPermission("android.permission.DUMP");
static const String16 sLocationHardwarePermission("android.permission.LOCATION_HARDWARE");
static const String16 sManageSensorsPermission("android.permission.MANAGE_SENSORS");

namespace {

int32_t nextRuntimeSensorHandle() {
    using ::aidl::android::hardware::sensors::ISensors;
    static int32_t nextHandle = ISensors::RUNTIME_SENSORS_HANDLE_BASE;
    if (nextHandle == ISensors::RUNTIME_SENSORS_HANDLE_END) {
        return -1;
    }
    return nextHandle++;
}

class RuntimeSensorCallbackProxy : public RuntimeSensor::SensorCallback {
 public:
    RuntimeSensorCallbackProxy(sp<SensorService::RuntimeSensorCallback> callback)
        : mCallback(std::move(callback)) {}
    status_t onConfigurationChanged(int handle, bool enabled, int64_t samplingPeriodNs,
                                    int64_t batchReportLatencyNs) override {
        return mCallback->onConfigurationChanged(handle, enabled, samplingPeriodNs,
                batchReportLatencyNs);
    }
 private:
    sp<SensorService::RuntimeSensorCallback> mCallback;
};

} // namespace

static bool isAutomotive() {
    sp<IServiceManager> serviceManager = defaultServiceManager();
    if (serviceManager.get() == nullptr) {
        ALOGE("%s: unable to access native ServiceManager", __func__);
        return false;
    }

    sp<content::pm::IPackageManagerNative> packageManager;
    sp<IBinder> binder = serviceManager->waitForService(String16("package_native"));
    packageManager = interface_cast<content::pm::IPackageManagerNative>(binder);
    if (packageManager == nullptr) {
        ALOGE("%s: unable to access native PackageManager", __func__);
        return false;
    }

    bool isAutomotive = false;
    binder::Status status =
        packageManager->hasSystemFeature(String16("android.hardware.type.automotive"), 0,
                                         &isAutomotive);
    if (!status.isOk()) {
        ALOGE("%s: hasSystemFeature failed: %s", __func__, status.exceptionMessage().c_str());
        return false;
    }

    return isAutomotive;
}

SensorService::SensorService()
    : mInitCheck(NO_INIT), mSocketBufferSize(SOCKET_BUFFER_SIZE_NON_BATCHED),
      mWakeLockAcquired(false), mLastReportedProxIsActive(false) {
    mUidPolicy = new UidPolicy(this);
    mSensorPrivacyPolicy = new SensorPrivacyPolicy(this);
    mMicSensorPrivacyPolicy = new MicrophonePrivacyPolicy(this);
}

int SensorService::registerRuntimeSensor(
        const sensor_t& sensor, int deviceId, sp<RuntimeSensorCallback> callback) {
    int handle = 0;
    while (handle == 0 || !mSensors.isNewHandle(handle)) {
        handle = nextRuntimeSensorHandle();
        if (handle < 0) {
            // Ran out of the dedicated range for runtime sensors.
            return handle;
        }
    }

    ALOGI("Registering runtime sensor handle 0x%x, type %d, name %s",
            handle, sensor.type, sensor.name);

    sp<RuntimeSensor::SensorCallback> runtimeSensorCallback(
            new RuntimeSensorCallbackProxy(callback));
    sensor_t runtimeSensor = sensor;
    // force the handle to be consistent
    runtimeSensor.handle = handle;
    auto si = std::make_shared<RuntimeSensor>(runtimeSensor, std::move(runtimeSensorCallback));

    Mutex::Autolock _l(mLock);
    if (!registerSensor(std::move(si), /* isDebug= */ false, /* isVirtual= */ false, deviceId)) {
        // The registration was unsuccessful.
        return mSensors.getNonSensor().getHandle();
    }

    if (mRuntimeSensorCallbacks.find(deviceId) == mRuntimeSensorCallbacks.end()) {
        mRuntimeSensorCallbacks.emplace(deviceId, callback);
    }

    if (mRuntimeSensorHandler == nullptr) {
        mRuntimeSensorEventBuffer =
                new sensors_event_t[SensorEventQueue::MAX_RECEIVE_BUFFER_EVENT_COUNT];
        mRuntimeSensorHandler = new RuntimeSensorHandler(this);
        // Use PRIORITY_URGENT_DISPLAY as the injected sensor events should be dispatched as soon as
        // possible, and also for consistency within the SensorService.
        mRuntimeSensorHandler->run("RuntimeSensorHandler", PRIORITY_URGENT_DISPLAY);
    }

    return handle;
}

status_t SensorService::unregisterRuntimeSensor(int handle) {
    ALOGI("Unregistering runtime sensor handle 0x%x disconnected", handle);
    int deviceId = getDeviceIdFromHandle(handle);
    {
        Mutex::Autolock _l(mLock);
        if (!unregisterDynamicSensorLocked(handle)) {
            ALOGE("Runtime sensor release error.");
            return UNKNOWN_ERROR;
        }
    }

    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    for (const sp<SensorEventConnection>& connection : connLock.getActiveConnections()) {
        connection->removeSensor(handle);
    }

    // If this was the last sensor for this device, remove its callback.
    bool deviceHasSensors = false;
    mSensors.forEachEntry(
            [&deviceId, &deviceHasSensors] (const SensorServiceUtil::SensorList::Entry& e) -> bool {
                if (e.deviceId == deviceId) {
                    deviceHasSensors = true;
                    return false;  // stop iterating
                }
                return true;
            });
    if (!deviceHasSensors) {
        mRuntimeSensorCallbacks.erase(deviceId);
    }
    return OK;
}

status_t SensorService::sendRuntimeSensorEvent(const sensors_event_t& event) {
    std::unique_lock<std::mutex> lock(mRutimeSensorThreadMutex);
    mRuntimeSensorEventQueue.push(event);
    mRuntimeSensorsCv.notify_all();
    return OK;
}

bool SensorService::initializeHmacKey() {
    int fd = open(SENSOR_SERVICE_HMAC_KEY_FILE, O_RDONLY|O_CLOEXEC);
    if (fd != -1) {
        int result = read(fd, sHmacGlobalKey, sizeof(sHmacGlobalKey));
        close(fd);
        if (result == sizeof(sHmacGlobalKey)) {
            return true;
        }
        ALOGW("Unable to read HMAC key; generating new one.");
    }

    if (RAND_bytes(sHmacGlobalKey, sizeof(sHmacGlobalKey)) == -1) {
        ALOGW("Can't generate HMAC key; dynamic sensor getId() will be wrong.");
        return false;
    }

    // We need to make sure this is only readable to us.
    bool wroteKey = false;
    mkdir(SENSOR_SERVICE_DIR, S_IRWXU);
    fd = open(SENSOR_SERVICE_HMAC_KEY_FILE, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,
              S_IRUSR|S_IWUSR);
    if (fd != -1) {
        int result = write(fd, sHmacGlobalKey, sizeof(sHmacGlobalKey));
        close(fd);
        wroteKey = (result == sizeof(sHmacGlobalKey));
    }
    if (wroteKey) {
        ALOGI("Generated new HMAC key.");
    } else {
        ALOGW("Unable to write HMAC key; dynamic sensor getId() will change "
              "after reboot.");
    }
    // Even if we failed to write the key we return true, because we did
    // initialize the HMAC key.
    return true;
}

// Set main thread to SCHED_FIFO to lower sensor event latency when system is under load
void SensorService::enableSchedFifoMode() {
    struct sched_param param = {0};
    param.sched_priority = SENSOR_SERVICE_SCHED_FIFO_PRIORITY;
    if (sched_setscheduler(getTid(), SCHED_FIFO | SCHED_RESET_ON_FORK, &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO for SensorService thread");
    }
}

void SensorService::onFirstRef() {
    ALOGD("nuSensorService starting...");
    SensorDevice& dev(SensorDevice::getInstance());

    sHmacGlobalKeyIsValid = initializeHmacKey();

    if (dev.initCheck() == NO_ERROR) {
        sensor_t const* list;
        ssize_t count = dev.getSensorList(&list);
        if (count > 0) {
            bool hasGyro = false, hasAccel = false, hasMag = false;
            bool hasGyroUncalibrated = false;
            bool hasAccelUncalibrated = false;
            uint32_t virtualSensorsNeeds =
                    (1<<SENSOR_TYPE_GRAVITY) |
                    (1<<SENSOR_TYPE_LINEAR_ACCELERATION) |
                    (1<<SENSOR_TYPE_ROTATION_VECTOR) |
                    (1<<SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR) |
                    (1<<SENSOR_TYPE_GAME_ROTATION_VECTOR);

            for (ssize_t i=0 ; i<count ; i++) {
                bool useThisSensor = true;

                switch (list[i].type) {
                    case SENSOR_TYPE_ACCELEROMETER:
                        hasAccel = true;
                        break;
                    case SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED:
                        hasAccelUncalibrated = true;
                        break;
                    case SENSOR_TYPE_MAGNETIC_FIELD:
                        hasMag = true;
                        break;
                    case SENSOR_TYPE_GYROSCOPE:
                        hasGyro = true;
                        break;
                    case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
                        hasGyroUncalibrated = true;
                        break;
                    case SENSOR_TYPE_DYNAMIC_SENSOR_META:
                        if (sensorservice_flags::dynamic_sensor_hal_reconnect_handling()) {
                            mDynamicMetaSensorHandle = list[i].handle;
                        }
                      break;
                    case SENSOR_TYPE_GRAVITY:
                    case SENSOR_TYPE_LINEAR_ACCELERATION:
                    case SENSOR_TYPE_ROTATION_VECTOR:
                    case SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR:
                    case SENSOR_TYPE_GAME_ROTATION_VECTOR:
                        if (IGNORE_HARDWARE_FUSION) {
                            useThisSensor = false;
                        } else {
                            virtualSensorsNeeds &= ~(1<<list[i].type);
                        }
                        break;
                    default:
                        break;
                }
                if (useThisSensor) {
                    if (list[i].type == SENSOR_TYPE_PROXIMITY) {
                        auto s = std::make_shared<ProximitySensor>(list[i], *this);
                        const int handle = s->getSensor().getHandle();
                        if (registerSensor(std::move(s))) {
                            mProxSensorHandles.push_back(handle);
                        }
                    } else {
                        registerSensor(std::make_shared<HardwareSensor>(list[i]));
                    }
                }
            }

            // it's safe to instantiate the SensorFusion object here
            // (it wants to be instantiated after h/w sensors have been
            // registered)
            SensorFusion::getInstance();

            if ((hasGyro || hasGyroUncalibrated) && hasAccel && hasMag) {
                // Add Android virtual sensors if they're not already
                // available in the HAL
                bool needRotationVector =
                        (virtualSensorsNeeds & (1<<SENSOR_TYPE_ROTATION_VECTOR)) != 0;
                registerVirtualSensor(std::make_shared<RotationVectorSensor>(),
                                      /* isDebug= */ !needRotationVector);
                registerVirtualSensor(std::make_shared<OrientationSensor>(),
                                      /* isDebug= */ !needRotationVector);

                // virtual debugging sensors are not for user
                registerVirtualSensor(std::make_shared<CorrectedGyroSensor>(list, count),
                                      /* isDebug= */ true);
                registerVirtualSensor(std::make_shared<GyroDriftSensor>(), /* isDebug= */ true);
            }

            if (hasAccel && (hasGyro || hasGyroUncalibrated)) {
                bool needGravitySensor = (virtualSensorsNeeds & (1<<SENSOR_TYPE_GRAVITY)) != 0;
                registerVirtualSensor(std::make_shared<GravitySensor>(list, count),
                                      /* isDebug= */ !needGravitySensor);

                bool needLinearAcceleration =
                        (virtualSensorsNeeds & (1<<SENSOR_TYPE_LINEAR_ACCELERATION)) != 0;
                registerVirtualSensor(std::make_shared<LinearAccelerationSensor>(list, count),
                                      /* isDebug= */ !needLinearAcceleration);

                bool needGameRotationVector =
                        (virtualSensorsNeeds & (1<<SENSOR_TYPE_GAME_ROTATION_VECTOR)) != 0;
                registerVirtualSensor(std::make_shared<GameRotationVectorSensor>(),
                                      /* isDebug= */ !needGameRotationVector);
            }

            if (hasAccel && hasMag) {
                bool needGeoMagRotationVector =
                        (virtualSensorsNeeds & (1<<SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR)) != 0;
                registerVirtualSensor(std::make_shared<GeoMagRotationVectorSensor>(),
                                      /* isDebug= */ !needGeoMagRotationVector);
            }

            if (isAutomotive()) {
                if (hasAccel) {
                    registerVirtualSensor(
                            std::make_shared<LimitedAxesImuSensor>(
                                    list, count, SENSOR_TYPE_ACCELEROMETER));
               }

               if (hasGyro) {
                    registerVirtualSensor(
                            std::make_shared<LimitedAxesImuSensor>(
                                    list, count, SENSOR_TYPE_GYROSCOPE));
               }

               if (hasAccelUncalibrated) {
                    registerVirtualSensor(
                            std::make_shared<LimitedAxesImuSensor>(
                                    list, count, SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED));
               }

               if (hasGyroUncalibrated) {
                    registerVirtualSensor(
                            std::make_shared<LimitedAxesImuSensor>(
                                    list, count, SENSOR_TYPE_GYROSCOPE_UNCALIBRATED));
               }
            }

            // Check if the device really supports batching by looking at the FIFO event
            // counts for each sensor.
            bool batchingSupported = false;
            mSensors.forEachSensor(
                    [&batchingSupported] (const Sensor& s) -> bool {
                        if (s.getFifoMaxEventCount() > 0) {
                            batchingSupported = true;
                        }
                        return !batchingSupported;
                    });

            if (batchingSupported) {
                // Increase socket buffer size to a max of 100 KB for batching capabilities.
                mSocketBufferSize = MAX_SOCKET_BUFFER_SIZE_BATCHED;
            } else {
                mSocketBufferSize = SOCKET_BUFFER_SIZE_NON_BATCHED;
            }

            // Compare the socketBufferSize value against the system limits and limit
            // it to maxSystemSocketBufferSize if necessary.
            FILE *fp = fopen("/proc/sys/net/core/wmem_max", "r");
            char line[128];
            if (fp != nullptr && fgets(line, sizeof(line), fp) != nullptr) {
                line[sizeof(line) - 1] = '\0';
                size_t maxSystemSocketBufferSize;
                sscanf(line, "%zu", &maxSystemSocketBufferSize);
                if (mSocketBufferSize > maxSystemSocketBufferSize) {
                    mSocketBufferSize = maxSystemSocketBufferSize;
                }
            }
            if (fp) {
                fclose(fp);
            }

            mWakeLockAcquired = false;
            mLooper = new Looper(false);
            const size_t minBufferSize = SensorEventQueue::MAX_RECEIVE_BUFFER_EVENT_COUNT;
            mSensorEventBuffer = new sensors_event_t[minBufferSize];
            mSensorEventScratch = new sensors_event_t[minBufferSize];
            mRuntimeSensorEventBuffer = nullptr;
            mMapFlushEventsToConnections = new wp<const SensorEventConnection> [minBufferSize];
            mCurrentOperatingMode = NORMAL;

            mNextSensorRegIndex = 0;
            for (int i = 0; i < SENSOR_REGISTRATIONS_BUF_SIZE; ++i) {
                mLastNSensorRegistrations.push();
            }

            mInitCheck = NO_ERROR;
            mAckReceiver = new SensorEventAckReceiver(this);
            mAckReceiver->run("SensorEventAckReceiver", PRIORITY_URGENT_DISPLAY);
            run("SensorService", PRIORITY_URGENT_DISPLAY);

            // priority can only be changed after run
            enableSchedFifoMode();

            // Start watching UID changes to apply policy.
            mUidPolicy->registerSelf();

            // Start watching sensor privacy changes
            mSensorPrivacyPolicy->registerSelf();

            // Start watching mic sensor privacy changes
            mMicSensorPrivacyPolicy->registerSelf();
        }
    }
}

void SensorService::onUidStateChanged(uid_t uid, UidState state) {
    SensorDevice& dev(SensorDevice::getInstance());

    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    for (const sp<SensorEventConnection>& conn : connLock.getActiveConnections()) {
        if (conn->getUid() == uid) {
            dev.setUidStateForConnection(conn.get(), state);
        }
    }

    for (const sp<SensorDirectConnection>& conn : connLock.getDirectConnections()) {
        if (conn->getUid() == uid) {
            // Update sensor subscriptions if needed
            bool hasAccess = hasSensorAccessLocked(conn->getUid(), conn->getOpPackageName());
            conn->onSensorAccessChanged(hasAccess);
        }
    }
    checkAndReportProxStateChangeLocked();
}

bool SensorService::hasSensorAccess(uid_t uid, const String16& opPackageName) {
    Mutex::Autolock _l(mLock);
    return hasSensorAccessLocked(uid, opPackageName);
}

bool SensorService::hasSensorAccessLocked(uid_t uid, const String16& opPackageName) {
    return !mSensorPrivacyPolicy->isSensorPrivacyEnabled()
        && isUidActive(uid) && !isOperationRestrictedLocked(opPackageName);
}

bool SensorService::registerSensor(std::shared_ptr<SensorInterface> s, bool isDebug, bool isVirtual,
                                   int deviceId) {
    const int handle = s->getSensor().getHandle();
    const int type = s->getSensor().getType();
    if (mSensors.add(handle, std::move(s), isDebug, isVirtual, deviceId)) {
        mRecentEvent.emplace(handle, new SensorServiceUtil::RecentEventLogger(type));
        return true;
    } else {
        LOG_FATAL("Failed to register sensor with handle %d", handle);
        return false;
    }
}

bool SensorService::registerDynamicSensorLocked(std::shared_ptr<SensorInterface> s, bool isDebug) {
    return registerSensor(std::move(s), isDebug);
}

bool SensorService::unregisterDynamicSensorLocked(int handle) {
    bool ret = mSensors.remove(handle);

    const auto i = mRecentEvent.find(handle);
    if (i != mRecentEvent.end()) {
        delete i->second;
        mRecentEvent.erase(i);
    }
    return ret;
}

bool SensorService::registerVirtualSensor(std::shared_ptr<SensorInterface> s, bool isDebug) {
    return registerSensor(std::move(s), isDebug, true);
}

SensorService::~SensorService() {
    for (auto && entry : mRecentEvent) {
        delete entry.second;
    }
    mUidPolicy->unregisterSelf();
    mSensorPrivacyPolicy->unregisterSelf();
    mMicSensorPrivacyPolicy->unregisterSelf();
}

status_t SensorService::dump(int fd, const Vector<String16>& args) {
    String8 result;
    if (!PermissionCache::checkCallingPermission(sDumpPermission)) {
        result.appendFormat("Permission Denial: can't dump SensorService from pid=%d, uid=%d\n",
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid());
    } else {
        bool privileged = IPCThreadState::self()->getCallingUid() == 0;
        if (args.size() > 2) {
           return INVALID_OPERATION;
        }
        if (args.size() > 0) {
            Mode targetOperatingMode = NORMAL;
            std::string inputStringMode = String8(args[0]).c_str();
            if (getTargetOperatingMode(inputStringMode, &targetOperatingMode)) {
              status_t error = changeOperatingMode(args, targetOperatingMode);
              // Dump the latest state only if no error was encountered.
              if (error != NO_ERROR) {
                return error;
              }
            }
        }

        ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
        // Run the following logic if a transition isn't requested above based on the input
        // argument parsing.
        if (args.size() == 1 && args[0] == String16("--proto")) {
            return dumpProtoLocked(fd, &connLock);
        } else if (!mSensors.hasAnySensor()) {
            result.append("No Sensors on the device\n");
            result.appendFormat("devInitCheck : %d\n", SensorDevice::getInstance().initCheck());
        } else {
            // Default dump the sensor list and debugging information.
            //
            timespec curTime;
            clock_gettime(CLOCK_REALTIME, &curTime);
            struct tm* timeinfo = localtime(&(curTime.tv_sec));
            result.appendFormat("Captured at: %02d:%02d:%02d.%03d\n", timeinfo->tm_hour,
                                timeinfo->tm_min, timeinfo->tm_sec, (int)ns2ms(curTime.tv_nsec));
            result.append("Sensor Device:\n");
            result.append(SensorDevice::getInstance().dump().c_str());

            result.append("Sensor List:\n");
            result.append(mSensors.dump().c_str());

            result.append("Fusion States:\n");
            SensorFusion::getInstance().dump(result);

            result.append("Recent Sensor events:\n");
            for (auto&& i : mRecentEvent) {
                std::shared_ptr<SensorInterface> s = getSensorInterfaceFromHandle(i.first);
                if (!i.second->isEmpty() && s != nullptr) {
                    if (privileged || s->getSensor().getRequiredPermission().empty()) {
                        i.second->setFormat("normal");
                    } else {
                        i.second->setFormat("mask_data");
                    }
                    // if there is events and sensor does not need special permission.
                    result.appendFormat("%s: ", s->getSensor().getName().c_str());
                    result.append(i.second->dump().c_str());
                }
            }

            result.append("Active sensors:\n");
            SensorDevice& dev = SensorDevice::getInstance();
            for (size_t i=0 ; i<mActiveSensors.size() ; i++) {
                int handle = mActiveSensors.keyAt(i);
                if (dev.isSensorActive(handle)) {
                    result.appendFormat("%s (handle=0x%08x, connections=%zu)\n",
                            getSensorName(handle).c_str(),
                            handle,
                            mActiveSensors.valueAt(i)->getNumConnections());
                }
            }

            result.appendFormat("Socket Buffer size = %zd events\n",
                                mSocketBufferSize/sizeof(sensors_event_t));
            result.appendFormat("WakeLock Status: %s \n", mWakeLockAcquired ? "acquired" :
                    "not held");
            result.appendFormat("Mode :");
            switch(mCurrentOperatingMode) {
               case NORMAL:
                   result.appendFormat(" NORMAL\n");
                   break;
               case RESTRICTED:
                   result.appendFormat(" RESTRICTED : %s\n", mAllowListedPackage.c_str());
                   break;
               case DATA_INJECTION:
                   result.appendFormat(" DATA_INJECTION : %s\n", mAllowListedPackage.c_str());
                   break;
               case REPLAY_DATA_INJECTION:
                   result.appendFormat(" REPLAY_DATA_INJECTION : %s\n",
                            mAllowListedPackage.c_str());
                   break;
               case HAL_BYPASS_REPLAY_DATA_INJECTION:
                   result.appendFormat(" HAL_BYPASS_REPLAY_DATA_INJECTION : %s\n",
                            mAllowListedPackage.c_str());
                   break;
               default:
                   result.appendFormat(" UNKNOWN\n");
                   break;
            }
            result.appendFormat("Sensor Privacy: %s\n",
                    mSensorPrivacyPolicy->isSensorPrivacyEnabled() ? "enabled" : "disabled");

            const auto& activeConnections = connLock.getActiveConnections();
            result.appendFormat("%zd open event connections\n", activeConnections.size());
            for (size_t i=0 ; i < activeConnections.size() ; i++) {
                result.appendFormat("Connection Number: %zu \n", i);
                activeConnections[i]->dump(result);
            }

            const auto& directConnections = connLock.getDirectConnections();
            result.appendFormat("%zd open direct connections\n", directConnections.size());
            for (size_t i = 0 ; i < directConnections.size() ; i++) {
                result.appendFormat("Direct connection %zu:\n", i);
                directConnections[i]->dump(result);
            }

            result.appendFormat("Previous Registrations:\n");
            // Log in the reverse chronological order.
            int currentIndex = (mNextSensorRegIndex - 1 + SENSOR_REGISTRATIONS_BUF_SIZE) %
                SENSOR_REGISTRATIONS_BUF_SIZE;
            const int startIndex = currentIndex;
            do {
                const SensorRegistrationInfo& reg_info = mLastNSensorRegistrations[currentIndex];
                if (SensorRegistrationInfo::isSentinel(reg_info)) {
                    // Ignore sentinel, proceed to next item.
                    currentIndex = (currentIndex - 1 + SENSOR_REGISTRATIONS_BUF_SIZE) %
                        SENSOR_REGISTRATIONS_BUF_SIZE;
                    continue;
                }
                result.appendFormat("%s\n", reg_info.dump(this).c_str());
                currentIndex = (currentIndex - 1 + SENSOR_REGISTRATIONS_BUF_SIZE) %
                        SENSOR_REGISTRATIONS_BUF_SIZE;
            } while(startIndex != currentIndex);
        }
    }
    write(fd, result.c_str(), result.size());
    return NO_ERROR;
}

/**
 * Dump debugging information as android.service.SensorServiceProto protobuf message using
 * ProtoOutputStream.
 *
 * See proto definition and some notes about ProtoOutputStream in
 * frameworks/base/core/proto/android/service/sensor_service.proto
 */
status_t SensorService::dumpProtoLocked(int fd, ConnectionSafeAutolock* connLock) const {
    using namespace service::SensorServiceProto;
    util::ProtoOutputStream proto;
    proto.write(INIT_STATUS, int(SensorDevice::getInstance().initCheck()));
    if (!mSensors.hasAnySensor()) {
        return proto.flush(fd) ? OK : UNKNOWN_ERROR;
    }
    const bool privileged = IPCThreadState::self()->getCallingUid() == 0;

    timespec curTime;
    clock_gettime(CLOCK_REALTIME, &curTime);
    proto.write(CURRENT_TIME_MS, curTime.tv_sec * 1000 + ns2ms(curTime.tv_nsec));

    // Write SensorDeviceProto
    uint64_t token = proto.start(SENSOR_DEVICE);
    SensorDevice::getInstance().dump(&proto);
    proto.end(token);

    // Write SensorListProto
    token = proto.start(SENSORS);
    mSensors.dump(&proto);
    proto.end(token);

    // Write SensorFusionProto
    token = proto.start(FUSION_STATE);
    SensorFusion::getInstance().dump(&proto);
    proto.end(token);

    // Write SensorEventsProto
    token = proto.start(SENSOR_EVENTS);
    for (auto&& i : mRecentEvent) {
        std::shared_ptr<SensorInterface> s = getSensorInterfaceFromHandle(i.first);
        if (!i.second->isEmpty() && s != nullptr) {
            i.second->setFormat(privileged || s->getSensor().getRequiredPermission().empty() ?
                    "normal" : "mask_data");
            const uint64_t mToken = proto.start(service::SensorEventsProto::RECENT_EVENTS_LOGS);
            proto.write(service::SensorEventsProto::RecentEventsLog::NAME,
                    std::string(s->getSensor().getName().c_str()));
            i.second->dump(&proto);
            proto.end(mToken);
        }
    }
    proto.end(token);

    // Write ActiveSensorProto
    SensorDevice& dev = SensorDevice::getInstance();
    for (size_t i=0 ; i<mActiveSensors.size() ; i++) {
        int handle = mActiveSensors.keyAt(i);
        if (dev.isSensorActive(handle)) {
            token = proto.start(ACTIVE_SENSORS);
            proto.write(service::ActiveSensorProto::NAME,
                    std::string(getSensorName(handle).c_str()));
            proto.write(service::ActiveSensorProto::HANDLE, handle);
            proto.write(service::ActiveSensorProto::NUM_CONNECTIONS,
                    int(mActiveSensors.valueAt(i)->getNumConnections()));
            proto.end(token);
        }
    }

    proto.write(SOCKET_BUFFER_SIZE, int(mSocketBufferSize));
    proto.write(SOCKET_BUFFER_SIZE_IN_EVENTS, int(mSocketBufferSize / sizeof(sensors_event_t)));
    proto.write(WAKE_LOCK_ACQUIRED, mWakeLockAcquired);

    switch(mCurrentOperatingMode) {
        case NORMAL:
            proto.write(OPERATING_MODE, OP_MODE_NORMAL);
            break;
        case RESTRICTED:
            proto.write(OPERATING_MODE, OP_MODE_RESTRICTED);
            proto.write(WHITELISTED_PACKAGE, std::string(mAllowListedPackage.c_str()));
            break;
        case DATA_INJECTION:
            proto.write(OPERATING_MODE, OP_MODE_DATA_INJECTION);
            proto.write(WHITELISTED_PACKAGE, std::string(mAllowListedPackage.c_str()));
            break;
        default:
            proto.write(OPERATING_MODE, OP_MODE_UNKNOWN);
    }
    proto.write(SENSOR_PRIVACY, mSensorPrivacyPolicy->isSensorPrivacyEnabled());

    // Write repeated SensorEventConnectionProto
    const auto& activeConnections = connLock->getActiveConnections();
    for (size_t i = 0; i < activeConnections.size(); i++) {
        token = proto.start(ACTIVE_CONNECTIONS);
        activeConnections[i]->dump(&proto);
        proto.end(token);
    }

    // Write repeated SensorDirectConnectionProto
    const auto& directConnections = connLock->getDirectConnections();
    for (size_t i = 0 ; i < directConnections.size() ; i++) {
        token = proto.start(DIRECT_CONNECTIONS);
        directConnections[i]->dump(&proto);
        proto.end(token);
    }

    // Write repeated SensorRegistrationInfoProto
    const int startIndex = mNextSensorRegIndex;
    int curr = startIndex;
    do {
        const SensorRegistrationInfo& reg_info = mLastNSensorRegistrations[curr];
        if (SensorRegistrationInfo::isSentinel(reg_info)) {
            // Ignore sentinel, proceed to next item.
            curr = (curr + 1 + SENSOR_REGISTRATIONS_BUF_SIZE) % SENSOR_REGISTRATIONS_BUF_SIZE;
            continue;
        }
        token = proto.start(PREVIOUS_REGISTRATIONS);
        reg_info.dump(&proto);
        proto.end(token);
        curr = (curr + 1 + SENSOR_REGISTRATIONS_BUF_SIZE) % SENSOR_REGISTRATIONS_BUF_SIZE;
    } while (startIndex != curr);

    return proto.flush(fd) ? OK : UNKNOWN_ERROR;
}

void SensorService::disableAllSensors() {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    disableAllSensorsLocked(&connLock);
}

void SensorService::disableAllSensorsLocked(ConnectionSafeAutolock* connLock) {
    SensorDevice& dev(SensorDevice::getInstance());
    for (const sp<SensorDirectConnection>& conn : connLock->getDirectConnections()) {
        bool hasAccess = hasSensorAccessLocked(conn->getUid(), conn->getOpPackageName());
        conn->onSensorAccessChanged(hasAccess);
    }
    dev.disableAllSensors();
    checkAndReportProxStateChangeLocked();
    // Clear all pending flush connections for all active sensors. If one of the active
    // connections has called flush() and the underlying sensor has been disabled before a
    // flush complete event is returned, we need to remove the connection from this queue.
    for (size_t i=0 ; i< mActiveSensors.size(); ++i) {
        mActiveSensors.valueAt(i)->clearAllPendingFlushConnections();
    }
}

void SensorService::enableAllSensors() {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    enableAllSensorsLocked(&connLock);
}

void SensorService::enableAllSensorsLocked(ConnectionSafeAutolock* connLock) {
    // sensors should only be enabled if the operating state is not restricted and sensor
    // privacy is not enabled.
    if (mCurrentOperatingMode == RESTRICTED || mSensorPrivacyPolicy->isSensorPrivacyEnabled()) {
        ALOGW("Sensors cannot be enabled: mCurrentOperatingMode = %d, sensor privacy = %s",
              mCurrentOperatingMode,
              mSensorPrivacyPolicy->isSensorPrivacyEnabled() ? "enabled" : "disabled");
        return;
    }
    SensorDevice& dev(SensorDevice::getInstance());
    dev.enableAllSensors();
    for (const sp<SensorDirectConnection>& conn : connLock->getDirectConnections()) {
        bool hasAccess = hasSensorAccessLocked(conn->getUid(), conn->getOpPackageName());
        conn->onSensorAccessChanged(hasAccess);
    }
    checkAndReportProxStateChangeLocked();
}

void SensorService::capRates() {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    for (const sp<SensorDirectConnection>& conn : connLock.getDirectConnections()) {
        conn->onMicSensorAccessChanged(true);
    }

    for (const sp<SensorEventConnection>& conn : connLock.getActiveConnections()) {
        conn->onMicSensorAccessChanged(true);
    }
}

void SensorService::uncapRates() {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    for (const sp<SensorDirectConnection>& conn : connLock.getDirectConnections()) {
        conn->onMicSensorAccessChanged(false);
    }

    for (const sp<SensorEventConnection>& conn : connLock.getActiveConnections()) {
        conn->onMicSensorAccessChanged(false);
    }
}

// NOTE: This is a remote API - make sure all args are validated
status_t SensorService::shellCommand(int in, int out, int err, Vector<String16>& args) {
    if (!checkCallingPermission(sManageSensorsPermission, nullptr, nullptr)) {
        return PERMISSION_DENIED;
    }
    if (args.size() == 0) {
      return BAD_INDEX;
    }
    if (in == BAD_TYPE || out == BAD_TYPE || err == BAD_TYPE) {
        return BAD_VALUE;
    }
    if (args[0] == String16("set-uid-state")) {
        return handleSetUidState(args, err);
    } else if (args[0] == String16("reset-uid-state")) {
        return handleResetUidState(args, err);
    } else if (args[0] == String16("get-uid-state")) {
        return handleGetUidState(args, out, err);
    } else if (args[0] == String16("unrestrict-ht")) {
        mHtRestricted = false;
        return NO_ERROR;
    } else if (args[0] == String16("restrict-ht")) {
        mHtRestricted = true;
        return NO_ERROR;
    } else if (args.size() == 1 && args[0] == String16("help")) {
        printHelp(out);
        return NO_ERROR;
    }
    printHelp(err);
    return BAD_VALUE;
}

static status_t getUidForPackage(String16 packageName, int userId, /*inout*/uid_t& uid, int err) {
    PermissionController pc;
    uid = pc.getPackageUid(packageName, 0);
    if (uid <= 0) {
        ALOGE("Unknown package: '%s'", String8(packageName).c_str());
        dprintf(err, "Unknown package: '%s'\n", String8(packageName).c_str());
        return BAD_VALUE;
    }

    if (userId < 0) {
        ALOGE("Invalid user: %d", userId);
        dprintf(err, "Invalid user: %d\n", userId);
        return BAD_VALUE;
    }

    uid = multiuser_get_uid(userId, uid);
    return NO_ERROR;
}

status_t SensorService::handleSetUidState(Vector<String16>& args, int err) {
    // Valid arg.size() is 3 or 5, args.size() is 5 with --user option.
    if (!(args.size() == 3 || args.size() == 5)) {
        printHelp(err);
        return BAD_VALUE;
    }

    bool active = false;
    if (args[2] == String16("active")) {
        active = true;
    } else if ((args[2] != String16("idle"))) {
        ALOGE("Expected active or idle but got: '%s'", String8(args[2]).c_str());
        return BAD_VALUE;
    }

    int userId = 0;
    if (args.size() == 5 && args[3] == String16("--user")) {
        userId = atoi(String8(args[4]));
    }

    uid_t uid;
    if (getUidForPackage(args[1], userId, uid, err) != NO_ERROR) {
        return BAD_VALUE;
    }

    mUidPolicy->addOverrideUid(uid, active);
    return NO_ERROR;
}

status_t SensorService::handleResetUidState(Vector<String16>& args, int err) {
    // Valid arg.size() is 2 or 4, args.size() is 4 with --user option.
    if (!(args.size() == 2 || args.size() == 4)) {
        printHelp(err);
        return BAD_VALUE;
    }

    int userId = 0;
    if (args.size() == 4 && args[2] == String16("--user")) {
        userId = atoi(String8(args[3]));
    }

    uid_t uid;
    if (getUidForPackage(args[1], userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }

    mUidPolicy->removeOverrideUid(uid);
    return NO_ERROR;
}

status_t SensorService::handleGetUidState(Vector<String16>& args, int out, int err) {
    // Valid arg.size() is 2 or 4, args.size() is 4 with --user option.
    if (!(args.size() == 2 || args.size() == 4)) {
        printHelp(err);
        return BAD_VALUE;
    }

    int userId = 0;
    if (args.size() == 4 && args[2] == String16("--user")) {
        userId = atoi(String8(args[3]));
    }

    uid_t uid;
    if (getUidForPackage(args[1], userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }

    if (mUidPolicy->isUidActive(uid)) {
        return dprintf(out, "active\n");
    } else {
        return dprintf(out, "idle\n");
    }
}

status_t SensorService::printHelp(int out) {
    return dprintf(out, "Sensor service commands:\n"
        "  get-uid-state <PACKAGE> [--user USER_ID] gets the uid state\n"
        "  set-uid-state <PACKAGE> <active|idle> [--user USER_ID] overrides the uid state\n"
        "  reset-uid-state <PACKAGE> [--user USER_ID] clears the uid state override\n"
        "  help print this message\n");
}

//TODO: move to SensorEventConnection later
void SensorService::cleanupAutoDisabledSensorLocked(const sp<SensorEventConnection>& connection,
        sensors_event_t const* buffer, const int count) {
    for (int i=0 ; i<count ; i++) {
        int handle = buffer[i].sensor;
        if (buffer[i].type == SENSOR_TYPE_META_DATA) {
            handle = buffer[i].meta_data.sensor;
        }
        if (connection->hasSensor(handle)) {
            std::shared_ptr<SensorInterface> si = getSensorInterfaceFromHandle(handle);
            // If this buffer has an event from a one_shot sensor and this connection is registered
            // for this particular one_shot sensor, try cleaning up the connection.
            if (si != nullptr &&
                si->getSensor().getReportingMode() == AREPORTING_MODE_ONE_SHOT) {
                si->autoDisable(connection.get(), handle);
                cleanupWithoutDisableLocked(connection, handle);
            }

        }
   }
}

void SensorService::sendEventsToAllClients(
    const std::vector<sp<SensorEventConnection>>& activeConnections,
    ssize_t count) {
   // Send our events to clients. Check the state of wake lock for each client
   // and release the lock if none of the clients need it.
   bool needsWakeLock = false;
   for (const sp<SensorEventConnection>& connection : activeConnections) {
       connection->sendEvents(mSensorEventBuffer, count, mSensorEventScratch,
                              mMapFlushEventsToConnections);
       needsWakeLock |= connection->needsWakeLock();
       // If the connection has one-shot sensors, it may be cleaned up after
       // first trigger. Early check for one-shot sensors.
       if (connection->hasOneShotSensors()) {
           cleanupAutoDisabledSensorLocked(connection, mSensorEventBuffer, count);
       }
   }

   if (mWakeLockAcquired && !needsWakeLock) {
        setWakeLockAcquiredLocked(false);
   }
}

void SensorService::disconnectDynamicSensor(
    int handle,
    const std::vector<sp<SensorEventConnection>>& activeConnections) {
   ALOGI("Dynamic sensor handle 0x%x disconnected", handle);
   SensorDevice::getInstance().handleDynamicSensorConnection(
       handle, false /*connected*/);
   if (!unregisterDynamicSensorLocked(handle)) {
        ALOGE("Dynamic sensor release error.");
   }
   for (const sp<SensorEventConnection>& connection : activeConnections) {
        connection->removeSensor(handle);
   }
}

void SensorService::handleDeviceReconnection(SensorDevice& device) {
    if (sensorservice_flags::dynamic_sensor_hal_reconnect_handling()) {
        const std::vector<sp<SensorEventConnection>> activeConnections =
                mConnectionHolder.lock(mLock).getActiveConnections();

        for (int32_t handle : device.getDynamicSensorHandles()) {
            if (mDynamicMetaSensorHandle.has_value()) {
                // Sending one event at a time to prevent the number of handle is more than the
                // buffer can hold.
                mSensorEventBuffer[0].type = SENSOR_TYPE_DYNAMIC_SENSOR_META;
                mSensorEventBuffer[0].sensor = *mDynamicMetaSensorHandle;
                mSensorEventBuffer[0].dynamic_sensor_meta.connected = false;
                mSensorEventBuffer[0].dynamic_sensor_meta.handle = handle;
                mMapFlushEventsToConnections[0] = nullptr;

                disconnectDynamicSensor(handle, activeConnections);
                sendEventsToAllClients(activeConnections, 1);
            } else {
                ALOGE("Failed to find mDynamicMetaSensorHandle during init.");
                break;
            }
        }
    }
    device.reconnect();
}

bool SensorService::threadLoop() {
    ALOGD("nuSensorService thread starting...");

    // each virtual sensor could generate an event per "real" event, that's why we need to size
    // numEventMax much smaller than MAX_RECEIVE_BUFFER_EVENT_COUNT.  in practice, this is too
    // aggressive, but guaranteed to be enough.
    const size_t vcount = mSensors.getVirtualSensors().size();
    const size_t minBufferSize = SensorEventQueue::MAX_RECEIVE_BUFFER_EVENT_COUNT;
    const size_t numEventMax = minBufferSize / (1 + vcount);

    SensorDevice& device(SensorDevice::getInstance());

    const int halVersion = device.getHalDeviceVersion();
    do {
        ssize_t count = device.poll(mSensorEventBuffer, numEventMax);
        if (count < 0) {
            if (count == DEAD_OBJECT && device.isReconnecting()) {
                handleDeviceReconnection(device);
                continue;
            } else {
                ALOGE("sensor poll failed (%s)", strerror(-count));
                break;
            }
        }

        // Reset sensors_event_t.flags to zero for all events in the buffer.
        for (int i = 0; i < count; i++) {
             mSensorEventBuffer[i].flags = 0;
        }
        ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);

        // Poll has returned. Hold a wakelock if one of the events is from a wake up sensor. The
        // rest of this loop is under a critical section protected by mLock. Acquiring a wakeLock,
        // sending events to clients (incrementing SensorEventConnection::mWakeLockRefCount) should
        // not be interleaved with decrementing SensorEventConnection::mWakeLockRefCount and
        // releasing the wakelock.
        uint32_t wakeEvents = 0;
        for (int i = 0; i < count; i++) {
            if (isWakeUpSensorEvent(mSensorEventBuffer[i])) {
                wakeEvents++;
            }
        }

        if (wakeEvents > 0) {
            if (!mWakeLockAcquired) {
                setWakeLockAcquiredLocked(true);
            }
            device.writeWakeLockHandled(wakeEvents);
        }
        recordLastValueLocked(mSensorEventBuffer, count);

        // handle virtual sensors
        if (count && vcount) {
            sensors_event_t const * const event = mSensorEventBuffer;
            if (!mActiveVirtualSensors.empty()) {
                size_t k = 0;
                SensorFusion& fusion(SensorFusion::getInstance());
                if (fusion.isEnabled()) {
                    for (size_t i=0 ; i<size_t(count) ; i++) {
                        fusion.process(event[i]);
                    }
                }
                for (size_t i=0 ; i<size_t(count) && k<minBufferSize ; i++) {
                    for (int handle : mActiveVirtualSensors) {
                        if (count + k >= minBufferSize) {
                            ALOGE("buffer too small to hold all events: "
                                    "count=%zd, k=%zu, size=%zu",
                                    count, k, minBufferSize);
                            break;
                        }
                        sensors_event_t out;
                        std::shared_ptr<SensorInterface> si = getSensorInterfaceFromHandle(handle);
                        if (si == nullptr) {
                            ALOGE("handle %d is not an valid virtual sensor", handle);
                            continue;
                        }

                        if (si->process(&out, event[i])) {
                            mSensorEventBuffer[count + k] = out;
                            k++;
                        }
                    }
                }
                if (k) {
                    // record the last synthesized values
                    recordLastValueLocked(&mSensorEventBuffer[count], k);
                    count += k;
                    sortEventBuffer(mSensorEventBuffer, count);
                }
            }
        }

        // handle backward compatibility for RotationVector sensor
        if (halVersion < SENSORS_DEVICE_API_VERSION_1_0) {
            for (int i = 0; i < count; i++) {
                if (mSensorEventBuffer[i].type == SENSOR_TYPE_ROTATION_VECTOR) {
                    // All the 4 components of the quaternion should be available
                    // No heading accuracy. Set it to -1
                    mSensorEventBuffer[i].data[4] = -1;
                }
            }
        }

        // Cache the list of active connections, since we use it in multiple places below but won't
        // modify it here
        const std::vector<sp<SensorEventConnection>> activeConnections = connLock.getActiveConnections();

        for (int i = 0; i < count; ++i) {
            // Map flush_complete_events in the buffer to SensorEventConnections which called flush
            // on the hardware sensor. mapFlushEventsToConnections[i] will be the
            // SensorEventConnection mapped to the corresponding flush_complete_event in
            // mSensorEventBuffer[i] if such a mapping exists (NULL otherwise).
            mMapFlushEventsToConnections[i] = nullptr;
            if (mSensorEventBuffer[i].type == SENSOR_TYPE_META_DATA) {
                const int sensor_handle = mSensorEventBuffer[i].meta_data.sensor;
                SensorRecord* rec = mActiveSensors.valueFor(sensor_handle);
                if (rec != nullptr) {
                    mMapFlushEventsToConnections[i] = rec->getFirstPendingFlushConnection();
                    rec->removeFirstPendingFlushConnection();
                }
            }
            // handle dynamic sensor meta events, process registration and unregistration of dynamic
            // sensor based on content of event.
            if (mSensorEventBuffer[i].type == SENSOR_TYPE_DYNAMIC_SENSOR_META) {
                if (mSensorEventBuffer[i].dynamic_sensor_meta.connected) {
                    int handle = mSensorEventBuffer[i].dynamic_sensor_meta.handle;
                    const sensor_t& dynamicSensor =
                            *(mSensorEventBuffer[i].dynamic_sensor_meta.sensor);
                    ALOGI("Dynamic sensor handle 0x%x connected, type %d, name %s",
                          handle, dynamicSensor.type, dynamicSensor.name);

                    if (mSensors.isNewHandle(handle)) {
                        const auto& uuid = mSensorEventBuffer[i].dynamic_sensor_meta.uuid;
                        sensor_t s = dynamicSensor;
                        // make sure the dynamic sensor flag is set
                        s.flags |= DYNAMIC_SENSOR_MASK;
                        // force the handle to be consistent
                        s.handle = handle;

                        auto si = std::make_shared<HardwareSensor>(s, uuid);

                        // This will release hold on dynamic sensor meta, so it should be called
                        // after Sensor object is created.
                        device.handleDynamicSensorConnection(handle, true /*connected*/);
                        registerDynamicSensorLocked(std::move(si));
                    } else {
                        ALOGE("Handle %d has been used, cannot use again before reboot.", handle);
                    }
                } else {
                    int handle = mSensorEventBuffer[i].dynamic_sensor_meta.handle;
                    disconnectDynamicSensor(handle, activeConnections);
                    if (sensorservice_flags::
                            sensor_service_clear_dynamic_sensor_data_at_the_end()) {
                      device.cleanupDisconnectedDynamicSensor(handle);
                    }
                }
            }
        }

        // Send our events to clients. Check the state of wake lock for each client and release the
        // lock if none of the clients need it.
        sendEventsToAllClients(activeConnections, count);
    } while (!Thread::exitPending());

    ALOGW("Exiting SensorService::threadLoop => aborting...");
    abort();
    return false;
}

void SensorService::processRuntimeSensorEvents() {
    size_t count = 0;
    const size_t maxBufferSize = SensorEventQueue::MAX_RECEIVE_BUFFER_EVENT_COUNT;

    {
        std::unique_lock<std::mutex> lock(mRutimeSensorThreadMutex);

        if (mRuntimeSensorEventQueue.empty()) {
            mRuntimeSensorsCv.wait(lock, [this] { return !mRuntimeSensorEventQueue.empty(); });
        }

        // Pop the events from the queue into the buffer until it's empty or the buffer is full.
        while (!mRuntimeSensorEventQueue.empty()) {
            if (count >= maxBufferSize) {
                ALOGE("buffer too small to hold all events: count=%zd, size=%zu", count,
                      maxBufferSize);
                break;
            }
            mRuntimeSensorEventBuffer[count] = mRuntimeSensorEventQueue.front();
            mRuntimeSensorEventQueue.pop();
            count++;
        }
    }

    if (count) {
        ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);

        recordLastValueLocked(mRuntimeSensorEventBuffer, count);
        sortEventBuffer(mRuntimeSensorEventBuffer, count);

        for (const sp<SensorEventConnection>& connection : connLock.getActiveConnections()) {
            connection->sendEvents(mRuntimeSensorEventBuffer, count, /* scratch= */ nullptr,
                                   /* mapFlushEventsToConnections= */ nullptr);
            if (connection->hasOneShotSensors()) {
                cleanupAutoDisabledSensorLocked(connection, mRuntimeSensorEventBuffer, count);
            }
        }
    }
}

sp<Looper> SensorService::getLooper() const {
    return mLooper;
}

void SensorService::resetAllWakeLockRefCounts() {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    for (const sp<SensorEventConnection>& connection : connLock.getActiveConnections()) {
        connection->resetWakeLockRefCount();
    }
    setWakeLockAcquiredLocked(false);
}

void SensorService::setWakeLockAcquiredLocked(bool acquire) {
    if (acquire) {
        if (!mWakeLockAcquired) {
            acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_NAME);
            mWakeLockAcquired = true;
        }
        mLooper->wake();
    } else {
        if (mWakeLockAcquired) {
            release_wake_lock(WAKE_LOCK_NAME);
            mWakeLockAcquired = false;
        }
    }
}

bool SensorService::isWakeLockAcquired() {
    Mutex::Autolock _l(mLock);
    return mWakeLockAcquired;
}

bool SensorService::SensorEventAckReceiver::threadLoop() {
    ALOGD("new thread SensorEventAckReceiver");
    sp<Looper> looper = mService->getLooper();
    do {
        bool wakeLockAcquired = mService->isWakeLockAcquired();
        int timeout = -1;
        if (wakeLockAcquired) timeout = 5000;
        int ret = looper->pollOnce(timeout);
        if (ret == ALOOPER_POLL_TIMEOUT) {
           mService->resetAllWakeLockRefCounts();
        }
    } while(!Thread::exitPending());
    return false;
}

bool SensorService::RuntimeSensorHandler::threadLoop() {
    ALOGD("new thread RuntimeSensorHandler");
    do {
        mService->processRuntimeSensorEvents();
    } while (!Thread::exitPending());
    return false;
}

void SensorService::recordLastValueLocked(
        const sensors_event_t* buffer, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (buffer[i].type == SENSOR_TYPE_META_DATA ||
            buffer[i].type == SENSOR_TYPE_DYNAMIC_SENSOR_META ||
            buffer[i].type == SENSOR_TYPE_ADDITIONAL_INFO) {
            continue;
        }

        auto logger = mRecentEvent.find(buffer[i].sensor);
        if (logger != mRecentEvent.end()) {
            logger->second->addEvent(buffer[i]);
        }
    }
}

void SensorService::sortEventBuffer(sensors_event_t* buffer, size_t count) {
    struct compar {
        static int cmp(void const* lhs, void const* rhs) {
            sensors_event_t const* l = static_cast<sensors_event_t const*>(lhs);
            sensors_event_t const* r = static_cast<sensors_event_t const*>(rhs);
            return l->timestamp - r->timestamp;
        }
    };
    qsort(buffer, count, sizeof(sensors_event_t), compar::cmp);
}

String8 SensorService::getSensorName(int handle) const {
    return mSensors.getName(handle);
}

String8 SensorService::getSensorStringType(int handle) const {
    return mSensors.getStringType(handle);
}

bool SensorService::isVirtualSensor(int handle) const {
    std::shared_ptr<SensorInterface> sensor = getSensorInterfaceFromHandle(handle);
    return sensor != nullptr && sensor->isVirtual();
}

bool SensorService::isWakeUpSensorEvent(const sensors_event_t& event) const {
    int handle = event.sensor;
    if (event.type == SENSOR_TYPE_META_DATA) {
        handle = event.meta_data.sensor;
    }
    std::shared_ptr<SensorInterface> sensor = getSensorInterfaceFromHandle(handle);
    return sensor != nullptr && sensor->getSensor().isWakeUpSensor();
}

int32_t SensorService::getIdFromUuid(const Sensor::uuid_t &uuid) const {
    if ((uuid.i64[0] == 0) && (uuid.i64[1] == 0)) {
        // UUID is not supported for this device.
        return 0;
    }
    if ((uuid.i64[0] == INT64_C(~0)) && (uuid.i64[1] == INT64_C(~0))) {
        // This sensor can be uniquely identified in the system by
        // the combination of its type and name.
        return -1;
    }

    // We have a dynamic sensor.

    if (!sHmacGlobalKeyIsValid) {
        // Rather than risk exposing UUIDs, we slow down dynamic sensors.
        ALOGW("HMAC key failure; dynamic sensor getId() will be wrong.");
        return 0;
    }

    // We want each app author/publisher to get a different ID, so that the
    // same dynamic sensor cannot be tracked across apps by multiple
    // authors/publishers.  So we use both our UUID and our User ID.
    // Note potential confusion:
    //     UUID => Universally Unique Identifier.
    //     UID  => User Identifier.
    // We refrain from using "uid" except as needed by API to try to
    // keep this distinction clear.

    auto appUserId = IPCThreadState::self()->getCallingUid();
    uint8_t uuidAndApp[sizeof(uuid) + sizeof(appUserId)];
    memcpy(uuidAndApp, &uuid, sizeof(uuid));
    memcpy(uuidAndApp + sizeof(uuid), &appUserId, sizeof(appUserId));

    // Now we use our key on our UUID/app combo to get the hash.
    uint8_t hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen;
    if (HMAC(EVP_sha256(),
             sHmacGlobalKey, sizeof(sHmacGlobalKey),
             uuidAndApp, sizeof(uuidAndApp),
             hash, &hashLen) == nullptr) {
        // Rather than risk exposing UUIDs, we slow down dynamic sensors.
        ALOGW("HMAC failure; dynamic sensor getId() will be wrong.");
        return 0;
    }

    int32_t id = 0;
    if (hashLen < sizeof(id)) {
        // We never expect this case, but out of paranoia, we handle it.
        // Our 'id' length is already quite small, we don't want the
        // effective length of it to be even smaller.
        // Rather than risk exposing UUIDs, we cripple dynamic sensors.
        ALOGW("HMAC insufficient; dynamic sensor getId() will be wrong.");
        return 0;
    }

    // This is almost certainly less than all of 'hash', but it's as secure
    // as we can be with our current 'id' length.
    memcpy(&id, hash, sizeof(id));

    // Note at the beginning of the function that we return the values of
    // 0 and -1 to represent special cases.  As a result, we can't return
    // those as dynamic sensor IDs.  If we happened to hash to one of those
    // values, we change 'id' so we report as a dynamic sensor, and not as
    // one of those special cases.
    if (id == -1) {
        id = -2;
    } else if (id == 0) {
        id = 1;
    }
    return id;
}

void SensorService::makeUuidsIntoIdsForSensorList(Vector<Sensor> &sensorList) const {
    for (auto &sensor : sensorList) {
        int32_t id = getIdFromUuid(sensor.getUuid());
        sensor.setId(id);
        // The sensor UUID must always be anonymized here for non privileged clients.
        // There is no other checks after this point before returning to client process.
        if (!isAudioServerOrSystemServerUid(IPCThreadState::self()->getCallingUid())) {
            sensor.anonymizeUuid();
        }
    }
}

Vector<Sensor> SensorService::getSensorList(const String16& opPackageName) {
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.sensors", value, "0");
    const Vector<Sensor>& initialSensorList = (atoi(value)) ?
            mSensors.getUserDebugSensors() : mSensors.getUserSensors();
    Vector<Sensor> accessibleSensorList;

    resetTargetSdkVersionCache(opPackageName);
    bool isCapped = isRateCappedBasedOnPermission(opPackageName);
    for (size_t i = 0; i < initialSensorList.size(); i++) {
        Sensor sensor = initialSensorList[i];
        if (isCapped && isSensorInCappedSet(sensor.getType())) {
            sensor.capMinDelayMicros(SENSOR_SERVICE_CAPPED_SAMPLING_PERIOD_NS / 1000);
            sensor.capHighestDirectReportRateLevel(SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL);
        }
        accessibleSensorList.add(sensor);
    }
    makeUuidsIntoIdsForSensorList(accessibleSensorList);
    return accessibleSensorList;
}

void SensorService::addSensorIfAccessible(const String16& opPackageName, const Sensor& sensor,
        Vector<Sensor>& accessibleSensorList) {
    if (canAccessSensor(sensor, "can't see", opPackageName)) {
        accessibleSensorList.add(sensor);
    } else if (sensor.getType() != SENSOR_TYPE_HEAD_TRACKER) {
        ALOGI("Skipped sensor %s because it requires permission %s and app op %" PRId32,
        sensor.getName().c_str(), sensor.getRequiredPermission().c_str(),
        sensor.getRequiredAppOp());
    }
}

Vector<Sensor> SensorService::getDynamicSensorList(const String16& opPackageName) {
    Vector<Sensor> accessibleSensorList;
    mSensors.forEachSensor(
            [this, &opPackageName, &accessibleSensorList] (const Sensor& sensor) -> bool {
                if (sensor.isDynamicSensor()) {
                    addSensorIfAccessible(opPackageName, sensor, accessibleSensorList);
                }
                return true;
            });
    makeUuidsIntoIdsForSensorList(accessibleSensorList);
    return accessibleSensorList;
}

Vector<Sensor> SensorService::getRuntimeSensorList(const String16& opPackageName, int deviceId) {
    Vector<Sensor> accessibleSensorList;
    mSensors.forEachEntry(
            [this, &opPackageName, deviceId, &accessibleSensorList] (
                    const SensorServiceUtil::SensorList::Entry& e) -> bool {
                if (e.deviceId == deviceId) {
                    addSensorIfAccessible(opPackageName, e.si->getSensor(), accessibleSensorList);
                }
                return true;
            });
    makeUuidsIntoIdsForSensorList(accessibleSensorList);
    return accessibleSensorList;
}

sp<ISensorEventConnection> SensorService::createSensorEventConnection(const String8& packageName,
        int requestedMode, const String16& opPackageName, const String16& attributionTag) {
    // Only 4 modes supported for a SensorEventConnection ... NORMAL, DATA_INJECTION,
    // REPLAY_DATA_INJECTION and HAL_BYPASS_REPLAY_DATA_INJECTION
    if (requestedMode != NORMAL && !isInjectionMode(requestedMode)) {
      ALOGE(
          "Failed to create sensor event connection: invalid request mode. "
          "requestMode: %d",
          requestedMode);
      return nullptr;
    }
    resetTargetSdkVersionCache(opPackageName);

    Mutex::Autolock _l(mLock);
    // To create a client in DATA_INJECTION mode to inject data, SensorService should already be
    // operating in DI mode.
    if (requestedMode == DATA_INJECTION) {
      if (mCurrentOperatingMode != DATA_INJECTION) {
        ALOGE(
            "Failed to create sensor event connection: sensor service not in "
            "DI mode when creating a client in DATA_INJECTION mode");
        return nullptr;
      }
      if (!isAllowListedPackage(packageName)) {
        ALOGE(
            "Failed to create sensor event connection: package %s not in "
            "allowed list for DATA_INJECTION mode",
            packageName.c_str());
        return nullptr;
      }
    }

    uid_t uid = IPCThreadState::self()->getCallingUid();
    pid_t pid = IPCThreadState::self()->getCallingPid();

    String8 connPackageName =
            (packageName == "") ? String8::format("unknown_package_pid_%d", pid) : packageName;
    String16 connOpPackageName =
            (opPackageName == String16("")) ? String16(connPackageName) : opPackageName;
    sp<SensorEventConnection> result(new SensorEventConnection(this, uid, connPackageName,
                                                               isInjectionMode(requestedMode),
                                                               connOpPackageName, attributionTag));
    if (isInjectionMode(requestedMode)) {
        mConnectionHolder.addEventConnectionIfNotPresent(result);
        // Add the associated file descriptor to the Looper for polling whenever there is data to
        // be injected.
        result->updateLooperRegistration(mLooper);
    }
    return result;
}

int SensorService::isDataInjectionEnabled() {
    Mutex::Autolock _l(mLock);
    return mCurrentOperatingMode == DATA_INJECTION;
}

int SensorService::isReplayDataInjectionEnabled() {
    Mutex::Autolock _l(mLock);
    return mCurrentOperatingMode == REPLAY_DATA_INJECTION;
}

int SensorService::isHalBypassReplayDataInjectionEnabled() {
    Mutex::Autolock _l(mLock);
    return mCurrentOperatingMode == HAL_BYPASS_REPLAY_DATA_INJECTION;
}

bool SensorService::isInjectionMode(int mode) {
    return (mode == DATA_INJECTION || mode == REPLAY_DATA_INJECTION ||
            mode == HAL_BYPASS_REPLAY_DATA_INJECTION);
}

sp<ISensorEventConnection> SensorService::createSensorDirectConnection(
        const String16& opPackageName, int deviceId, uint32_t size, int32_t type, int32_t format,
        const native_handle *resource) {
    resetTargetSdkVersionCache(opPackageName);
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);

    // No new direct connections are allowed when sensor privacy is enabled
    if (mSensorPrivacyPolicy->isSensorPrivacyEnabled()) {
        ALOGE("Cannot create new direct connections when sensor privacy is enabled");
        return nullptr;
    }

    struct sensors_direct_mem_t mem = {
        .type = type,
        .format = format,
        .size = size,
        .handle = resource,
    };
    uid_t uid = IPCThreadState::self()->getCallingUid();

    if (mem.handle == nullptr) {
        ALOGE("Failed to clone resource handle");
        return nullptr;
    }

    // check format
    if (format != SENSOR_DIRECT_FMT_SENSORS_EVENT) {
        ALOGE("Direct channel format %d is unsupported!", format);
        return nullptr;
    }

    // check for duplication
    for (const sp<SensorDirectConnection>& connection : connLock.getDirectConnections()) {
        if (connection->isEquivalent(&mem)) {
            ALOGE("Duplicate create channel request for the same share memory");
            return nullptr;
        }
    }

    // check specific to memory type
    switch(type) {
        case SENSOR_DIRECT_MEM_TYPE_ASHMEM: { // channel backed by ashmem
            if (resource->numFds < 1) {
                ALOGE("Ashmem direct channel requires a memory region to be supplied");
                android_errorWriteLog(0x534e4554, "70986337");  // SafetyNet
                return nullptr;
            }
            int fd = resource->data[0];
            if (!ashmem_valid(fd)) {
                ALOGE("Supplied Ashmem memory region is invalid");
                return nullptr;
            }

            int size2 = ashmem_get_size_region(fd);
            // check size consistency
            if (size2 < static_cast<int64_t>(size)) {
                ALOGE("Ashmem direct channel size %" PRIu32 " greater than shared memory size %d",
                      size, size2);
                return nullptr;
            }
            break;
        }
        case SENSOR_DIRECT_MEM_TYPE_GRALLOC:
            // no specific checks for gralloc
            break;
        default:
            ALOGE("Unknown direct connection memory type %d", type);
            return nullptr;
    }

    native_handle_t *clone = native_handle_clone(resource);
    if (!clone) {
        return nullptr;
    }
    native_handle_set_fdsan_tag(clone);

    sp<SensorDirectConnection> conn;
    int channelHandle = 0;
    if (deviceId == RuntimeSensor::DEFAULT_DEVICE_ID) {
        SensorDevice& dev(SensorDevice::getInstance());
        channelHandle = dev.registerDirectChannel(&mem);
    } else {
        auto runtimeSensorCallback = mRuntimeSensorCallbacks.find(deviceId);
        if (runtimeSensorCallback == mRuntimeSensorCallbacks.end()) {
            ALOGE("Runtime sensor callback for deviceId %d not found", deviceId);
        } else {
            int fd = dup(clone->data[0]);
            channelHandle = runtimeSensorCallback->second->onDirectChannelCreated(fd);
        }
    }

    if (channelHandle <= 0) {
        ALOGE("SensorDevice::registerDirectChannel returns %d", channelHandle);
    } else {
        mem.handle = clone;
        IPCThreadState* thread = IPCThreadState::self();
        pid_t pid = (thread != nullptr) ? thread->getCallingPid() : -1;
        conn = new SensorDirectConnection(this, uid, pid, &mem, channelHandle, opPackageName,
                                          deviceId);
    }

    if (conn == nullptr) {
        native_handle_close_with_tag(clone);
        native_handle_delete(clone);
    } else {
        // add to list of direct connections
        // sensor service should never hold pointer or sp of SensorDirectConnection object.
        mConnectionHolder.addDirectConnection(conn);
    }
    return conn;
}

int SensorService::configureRuntimeSensorDirectChannel(
        int sensorHandle, const SensorDirectConnection* c, const sensors_direct_cfg_t* config) {
    int deviceId = c->getDeviceId();
    int sensorDeviceId = getDeviceIdFromHandle(sensorHandle);
    if (sensorDeviceId != c->getDeviceId()) {
        ALOGE("Cannot configure direct channel created for device %d with a sensor that belongs "
              "to device %d", c->getDeviceId(), sensorDeviceId);
        return BAD_VALUE;
    }
    auto runtimeSensorCallback = mRuntimeSensorCallbacks.find(deviceId);
    if (runtimeSensorCallback == mRuntimeSensorCallbacks.end()) {
        ALOGE("Runtime sensor callback for deviceId %d not found", deviceId);
        return BAD_VALUE;
    }
    return runtimeSensorCallback->second->onDirectChannelConfigured(
            c->getHalChannelHandle(), sensorHandle, config->rate_level);
}

int SensorService::setOperationParameter(
            int32_t handle, int32_t type,
            const Vector<float> &floats, const Vector<int32_t> &ints) {
    Mutex::Autolock _l(mLock);

    if (!checkCallingPermission(sLocationHardwarePermission, nullptr, nullptr)) {
        return PERMISSION_DENIED;
    }

    bool isFloat = true;
    bool isCustom = false;
    size_t expectSize = INT32_MAX;
    switch (type) {
        case AINFO_LOCAL_GEOMAGNETIC_FIELD:
            isFloat = true;
            expectSize = 3;
            break;
        case AINFO_LOCAL_GRAVITY:
            isFloat = true;
            expectSize = 1;
            break;
        case AINFO_DOCK_STATE:
        case AINFO_HIGH_PERFORMANCE_MODE:
        case AINFO_MAGNETIC_FIELD_CALIBRATION:
            isFloat = false;
            expectSize = 1;
            break;
        default:
            // CUSTOM events must only contain float data; it may have variable size
            if (type < AINFO_CUSTOM_START || type >= AINFO_DEBUGGING_START ||
                    ints.size() ||
                    sizeof(additional_info_event_t::data_float)/sizeof(float) < floats.size() ||
                    handle < 0) {
                return BAD_VALUE;
            }
            isFloat = true;
            isCustom = true;
            expectSize = floats.size();
            break;
    }

    if (!isCustom && handle != -1) {
        return BAD_VALUE;
    }

    // three events: first one is begin tag, last one is end tag, the one in the middle
    // is the payload.
    sensors_event_t event[3];
    int64_t timestamp = elapsedRealtimeNano();
    for (sensors_event_t* i = event; i < event + 3; i++) {
        *i = (sensors_event_t) {
            .version = sizeof(sensors_event_t),
            .sensor = handle,
            .type = SENSOR_TYPE_ADDITIONAL_INFO,
            .timestamp = timestamp++,
            .additional_info = (additional_info_event_t) {
                .serial = 0
            }
        };
    }

    event[0].additional_info.type = AINFO_BEGIN;
    event[1].additional_info.type = type;
    event[2].additional_info.type = AINFO_END;

    if (isFloat) {
        if (floats.size() != expectSize) {
            return BAD_VALUE;
        }
        for (size_t i = 0; i < expectSize; ++i) {
            event[1].additional_info.data_float[i] = floats[i];
        }
    } else {
        if (ints.size() != expectSize) {
            return BAD_VALUE;
        }
        for (size_t i = 0; i < expectSize; ++i) {
            event[1].additional_info.data_int32[i] = ints[i];
        }
    }

    SensorDevice& dev(SensorDevice::getInstance());
    for (sensors_event_t* i = event; i < event + 3; i++) {
        int ret = dev.injectSensorData(i);
        if (ret != NO_ERROR) {
            return ret;
        }
    }
    return NO_ERROR;
}

status_t SensorService::resetToNormalMode() {
    Mutex::Autolock _l(mLock);
    return resetToNormalModeLocked();
}

status_t SensorService::resetToNormalModeLocked() {
    SensorDevice& dev(SensorDevice::getInstance());
    status_t err = dev.setMode(NORMAL);
    if (err == NO_ERROR) {
        mCurrentOperatingMode = NORMAL;
        dev.enableAllSensors();
        checkAndReportProxStateChangeLocked();
    }
    return err;
}

void SensorService::cleanupConnection(SensorEventConnection* c) {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    const wp<SensorEventConnection> connection(c);
    size_t size = mActiveSensors.size();
    ALOGD_IF(DEBUG_CONNECTIONS, "%zu active sensors", size);
    for (size_t i=0 ; i<size ; ) {
        int handle = mActiveSensors.keyAt(i);
        if (c->hasSensor(handle)) {
            ALOGD_IF(DEBUG_CONNECTIONS, "%zu: disabling handle=0x%08x", i, handle);
            std::shared_ptr<SensorInterface> sensor = getSensorInterfaceFromHandle(handle);
            if (sensor != nullptr) {
                sensor->activate(c, false);
            } else {
                ALOGE("sensor interface of handle=0x%08x is null!", handle);
            }
            if (c->removeSensor(handle)) {
                BatteryService::disableSensor(c->getUid(), handle);
            }
        }
        SensorRecord* rec = mActiveSensors.valueAt(i);
        ALOGE_IF(!rec, "mActiveSensors[%zu] is null (handle=0x%08x)!", i, handle);
        ALOGD_IF(DEBUG_CONNECTIONS,
                "removing connection %p for sensor[%zu].handle=0x%08x",
                c, i, handle);

        if (rec && rec->removeConnection(connection)) {
            ALOGD_IF(DEBUG_CONNECTIONS, "... and it was the last connection");
            mActiveSensors.removeItemsAt(i, 1);
            mActiveVirtualSensors.erase(handle);
            delete rec;
            size--;
        } else {
            i++;
        }
    }
    c->updateLooperRegistration(mLooper);
    mConnectionHolder.removeEventConnection(connection);
    if (c->needsWakeLock()) {
        checkWakeLockStateLocked(&connLock);
    }

    SensorDevice& dev(SensorDevice::getInstance());
    dev.notifyConnectionDestroyed(c);
}

void SensorService::cleanupConnection(SensorDirectConnection* c) {
    Mutex::Autolock _l(mLock);

    int deviceId = c->getDeviceId();
    if (deviceId == RuntimeSensor::DEFAULT_DEVICE_ID) {
        SensorDevice& dev(SensorDevice::getInstance());
        dev.unregisterDirectChannel(c->getHalChannelHandle());
    } else {
        auto runtimeSensorCallback = mRuntimeSensorCallbacks.find(deviceId);
        if (runtimeSensorCallback != mRuntimeSensorCallbacks.end()) {
            runtimeSensorCallback->second->onDirectChannelDestroyed(c->getHalChannelHandle());
        } else {
            ALOGE("Runtime sensor callback for deviceId %d not found", deviceId);
        }
    }
    mConnectionHolder.removeDirectConnection(c);
}

void SensorService::checkAndReportProxStateChangeLocked() {
    if (mProxSensorHandles.empty()) return;

    SensorDevice& dev(SensorDevice::getInstance());
    bool isActive = false;
    for (auto& sensor : mProxSensorHandles) {
        if (dev.isSensorActive(sensor)) {
            isActive = true;
            break;
        }
    }
    if (isActive != mLastReportedProxIsActive) {
        notifyProximityStateLocked(isActive, mProximityActiveListeners);
        mLastReportedProxIsActive = isActive;
    }
}

void SensorService::notifyProximityStateLocked(
        const bool isActive,
        const std::vector<sp<ProximityActiveListener>>& listeners) {
    const uint64_t mySeq = ++curProxCallbackSeq;
    std::thread t([isActive, mySeq, listenersCopy = listeners]() {
        while (completedCallbackSeq.load() != mySeq - 1)
            std::this_thread::sleep_for(1ms);
        for (auto& listener : listenersCopy)
            listener->onProximityActive(isActive);
        completedCallbackSeq++;
    });
    t.detach();
}

status_t SensorService::addProximityActiveListener(const sp<ProximityActiveListener>& callback) {
    if (callback == nullptr) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mLock);

    // Check if the callback was already added.
    for (const auto& cb : mProximityActiveListeners) {
        if (cb == callback) {
            return ALREADY_EXISTS;
        }
    }

    mProximityActiveListeners.push_back(callback);
    std::vector<sp<ProximityActiveListener>> listener(1, callback);
    notifyProximityStateLocked(mLastReportedProxIsActive, listener);
    return OK;
}

status_t SensorService::removeProximityActiveListener(
        const sp<ProximityActiveListener>& callback) {
    if (callback == nullptr) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mLock);

    for (auto iter = mProximityActiveListeners.begin();
         iter != mProximityActiveListeners.end();
         ++iter) {
        if (*iter == callback) {
            mProximityActiveListeners.erase(iter);
            return OK;
        }
    }
    return NAME_NOT_FOUND;
}

std::shared_ptr<SensorInterface> SensorService::getSensorInterfaceFromHandle(int handle) const {
    return mSensors.getInterface(handle);
}

int SensorService::getDeviceIdFromHandle(int handle) const {
    int deviceId = RuntimeSensor::DEFAULT_DEVICE_ID;
    mSensors.forEachEntry(
            [&deviceId, handle] (const SensorServiceUtil::SensorList::Entry& e) -> bool {
                if (e.si->getSensor().getHandle() == handle) {
                    deviceId = e.deviceId;
                    return false;  // stop iterating
                }
                return true;
            });
    return deviceId;
}

status_t SensorService::enable(const sp<SensorEventConnection>& connection,
        int handle, nsecs_t samplingPeriodNs, nsecs_t maxBatchReportLatencyNs, int reservedFlags,
        const String16& opPackageName) {
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    std::shared_ptr<SensorInterface> sensor = getSensorInterfaceFromHandle(handle);
    if (sensor == nullptr ||
        !canAccessSensor(sensor->getSensor(), "Tried enabling", opPackageName)) {
        return BAD_VALUE;
    }

    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    if (mCurrentOperatingMode != NORMAL &&
        !isInjectionMode(mCurrentOperatingMode) &&
        !isAllowListedPackage(connection->getPackageName())) {
      return INVALID_OPERATION;
    }

    SensorRecord* rec = mActiveSensors.valueFor(handle);
    if (rec == nullptr) {
        rec = new SensorRecord(connection);
        mActiveSensors.add(handle, rec);
        if (sensor->isVirtual()) {
            mActiveVirtualSensors.emplace(handle);
        }

        // There was no SensorRecord for this sensor which means it was previously disabled. Mark
        // the recent event as stale to ensure that the previous event is not sent to a client. This
        // ensures on-change events that were generated during a previous sensor activation are not
        // erroneously sent to newly connected clients, especially if a second client registers for
        // an on-change sensor before the first client receives the updated event. Once an updated
        // event is received, the recent events will be marked as current, and any new clients will
        // immediately receive the most recent event.
        if (sensor->getSensor().getReportingMode() == AREPORTING_MODE_ON_CHANGE) {
            auto logger = mRecentEvent.find(handle);
            if (logger != mRecentEvent.end()) {
                logger->second->setLastEventStale();
            }
        }
    } else {
        if (rec->addConnection(connection)) {
            // this sensor is already activated, but we are adding a connection that uses it.
            // Immediately send down the last known value of the requested sensor if it's not a
            // "continuous" sensor.
            if (sensor->getSensor().getReportingMode() == AREPORTING_MODE_ON_CHANGE) {
                // NOTE: The wake_up flag of this event may get set to
                // WAKE_UP_SENSOR_EVENT_NEEDS_ACK if this is a wake_up event.

                auto logger = mRecentEvent.find(handle);
                if (logger != mRecentEvent.end()) {
                    sensors_event_t event;
                    // Verify that the last sensor event was generated from the current activation
                    // of the sensor. If not, it is possible for an on-change sensor to receive a
                    // sensor event that is stale if two clients re-activate the sensor
                    // simultaneously.
                    if(logger->second->populateLastEventIfCurrent(&event)) {
                        event.sensor = handle;
                        if (event.version == sizeof(sensors_event_t)) {
                            if (isWakeUpSensorEvent(event) && !mWakeLockAcquired) {
                                setWakeLockAcquiredLocked(true);
                            }
                            connection->sendEvents(&event, 1, nullptr);
                            if (!connection->needsWakeLock() && mWakeLockAcquired) {
                                checkWakeLockStateLocked(&connLock);
                            }
                        }
                    }
                }
            }
        }
    }

    if (connection->addSensor(handle)) {
        BatteryService::enableSensor(connection->getUid(), handle);
        // the sensor was added (which means it wasn't already there)
        // so, see if this connection becomes active
        mConnectionHolder.addEventConnectionIfNotPresent(connection);
    } else {
        ALOGW("sensor %08x already enabled in connection %p (ignoring)",
            handle, connection.get());
    }

    // Check maximum delay for the sensor.
    nsecs_t maxDelayNs = sensor->getSensor().getMaxDelay() * 1000LL;
    if (maxDelayNs > 0 && (samplingPeriodNs > maxDelayNs)) {
        samplingPeriodNs = maxDelayNs;
    }

    nsecs_t minDelayNs = sensor->getSensor().getMinDelayNs();
    if (samplingPeriodNs < minDelayNs) {
        samplingPeriodNs = minDelayNs;
    }

    ALOGD_IF(DEBUG_CONNECTIONS, "Calling batch handle==%d flags=%d"
                                "rate=%" PRId64 " timeout== %" PRId64"",
             handle, reservedFlags, samplingPeriodNs, maxBatchReportLatencyNs);

    status_t err = sensor->batch(connection.get(), handle, 0, samplingPeriodNs,
                                 maxBatchReportLatencyNs);

    // Call flush() before calling activate() on the sensor. Wait for a first
    // flush complete event before sending events on this connection. Ignore
    // one-shot sensors which don't support flush(). Ignore on-change sensors
    // to maintain the on-change logic (any on-change events except the initial
    // one should be trigger by a change in value). Also if this sensor isn't
    // already active, don't call flush().
    if (err == NO_ERROR &&
            sensor->getSensor().getReportingMode() == AREPORTING_MODE_CONTINUOUS &&
            rec->getNumConnections() > 1) {
        connection->setFirstFlushPending(handle, true);
        status_t err_flush = sensor->flush(connection.get(), handle);
        // Flush may return error if the underlying h/w sensor uses an older HAL.
        if (err_flush == NO_ERROR) {
            rec->addPendingFlushConnection(connection.get());
        } else {
            connection->setFirstFlushPending(handle, false);
        }
    }

    if (err == NO_ERROR) {
        ALOGD_IF(DEBUG_CONNECTIONS, "Calling activate on %d", handle);
        err = sensor->activate(connection.get(), true);
    }

    if (err == NO_ERROR) {
        connection->updateLooperRegistration(mLooper);

        if (sensor->getSensor().getRequiredPermission().size() > 0 &&
                sensor->getSensor().getRequiredAppOp() >= 0) {
            connection->mHandleToAppOp[handle] = sensor->getSensor().getRequiredAppOp();
        }
    }

    if (err != NO_ERROR) {
        // batch/activate has failed, reset our state.
        cleanupWithoutDisableLocked(connection, handle);
    }

    mLastNSensorRegistrations.editItemAt(mNextSensorRegIndex) =
            SensorRegistrationInfo(handle, connection->getPackageName(), samplingPeriodNs,
                                   maxBatchReportLatencyNs, /*activate=*/ true, err);
    mNextSensorRegIndex = (mNextSensorRegIndex + 1) % SENSOR_REGISTRATIONS_BUF_SIZE;
    return err;
}

status_t SensorService::disable(const sp<SensorEventConnection>& connection, int handle) {
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    Mutex::Autolock _l(mLock);
    status_t err = cleanupWithoutDisableLocked(connection, handle);
    if (err == NO_ERROR) {
        std::shared_ptr<SensorInterface> sensor = getSensorInterfaceFromHandle(handle);
        err = sensor != nullptr ? sensor->activate(connection.get(), false) : status_t(BAD_VALUE);
    }
    mLastNSensorRegistrations.editItemAt(mNextSensorRegIndex) =
            SensorRegistrationInfo(handle, connection->getPackageName(), 0, 0, /*activate=*/ false, err);
    mNextSensorRegIndex = (mNextSensorRegIndex + 1) % SENSOR_REGISTRATIONS_BUF_SIZE;
    return err;
}

status_t SensorService::cleanupWithoutDisable(
        const sp<SensorEventConnection>& connection, int handle) {
    Mutex::Autolock _l(mLock);
    return cleanupWithoutDisableLocked(connection, handle);
}

status_t SensorService::cleanupWithoutDisableLocked(
        const sp<SensorEventConnection>& connection, int handle) {
    SensorRecord* rec = mActiveSensors.valueFor(handle);
    if (rec) {
        // see if this connection becomes inactive
        if (connection->removeSensor(handle)) {
            BatteryService::disableSensor(connection->getUid(), handle);
        }
        if (connection->hasAnySensor() == false) {
            connection->updateLooperRegistration(mLooper);
            mConnectionHolder.removeEventConnection(connection);
        }
        // see if this sensor becomes inactive
        if (rec->removeConnection(connection)) {
            mActiveSensors.removeItem(handle);
            mActiveVirtualSensors.erase(handle);
            delete rec;
        }
        return NO_ERROR;
    }
    return BAD_VALUE;
}

status_t SensorService::setEventRate(const sp<SensorEventConnection>& connection,
        int handle, nsecs_t ns, const String16& opPackageName) {
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    std::shared_ptr<SensorInterface> sensor = getSensorInterfaceFromHandle(handle);
    if (sensor == nullptr ||
        !canAccessSensor(sensor->getSensor(), "Tried configuring", opPackageName)) {
        return BAD_VALUE;
    }

    if (ns < 0)
        return BAD_VALUE;

    nsecs_t minDelayNs = sensor->getSensor().getMinDelayNs();
    if (ns < minDelayNs) {
        ns = minDelayNs;
    }

    return sensor->setDelay(connection.get(), handle, ns);
}

status_t SensorService::flushSensor(const sp<SensorEventConnection>& connection,
        const String16& opPackageName) {
    if (mInitCheck != NO_ERROR) return mInitCheck;
    SensorDevice& dev(SensorDevice::getInstance());
    const int halVersion = dev.getHalDeviceVersion();
    status_t err(NO_ERROR);
    Mutex::Autolock _l(mLock);
    // Loop through all sensors for this connection and call flush on each of them.
    for (int handle : connection->getActiveSensorHandles()) {
        std::shared_ptr<SensorInterface> sensor = getSensorInterfaceFromHandle(handle);
        if (sensor == nullptr) {
            continue;
        }
        if (sensor->getSensor().getReportingMode() == AREPORTING_MODE_ONE_SHOT) {
            ALOGE("flush called on a one-shot sensor");
            err = INVALID_OPERATION;
            continue;
        }
        if (halVersion <= SENSORS_DEVICE_API_VERSION_1_0 || isVirtualSensor(handle)) {
            // For older devices just increment pending flush count which will send a trivial
            // flush complete event.
            if (!connection->incrementPendingFlushCountIfHasAccess(handle)) {
                ALOGE("flush called on an inaccessible sensor");
                err = INVALID_OPERATION;
            }
        } else {
            if (!canAccessSensor(sensor->getSensor(), "Tried flushing", opPackageName)) {
                err = INVALID_OPERATION;
                continue;
            }
            status_t err_flush = sensor->flush(connection.get(), handle);
            if (err_flush == NO_ERROR) {
                SensorRecord* rec = mActiveSensors.valueFor(handle);
                if (rec != nullptr) rec->addPendingFlushConnection(connection);
            }
            err = (err_flush != NO_ERROR) ? err_flush : err;
        }
    }
    return err;
}

bool SensorService::canAccessSensor(const Sensor& sensor, const char* operation,
        const String16& opPackageName) {
    // Special case for Head Tracker sensor type: currently restricted to system usage only, unless
    // the restriction is specially lifted for testing
    if (sensor.getType() == SENSOR_TYPE_HEAD_TRACKER &&
            !isAudioServerOrSystemServerUid(IPCThreadState::self()->getCallingUid())) {
        if (!mHtRestricted) {
            ALOGI("Permitting access to HT sensor type outside system (%s)",
                  String8(opPackageName).c_str());
        } else {
            ALOGW("%s %s a sensor (%s) as a non-system client", String8(opPackageName).c_str(),
                  operation, sensor.getName().c_str());
            return false;
        }
    }

    // Check if a permission is required for this sensor
    if (sensor.getRequiredPermission().length() <= 0) {
        return true;
    }

    const int32_t opCode = sensor.getRequiredAppOp();
    int targetSdkVersion = getTargetSdkVersion(opPackageName);

    bool canAccess = false;
    if (targetSdkVersion > 0 && targetSdkVersion <= __ANDROID_API_P__ &&
            (sensor.getType() == SENSOR_TYPE_STEP_COUNTER ||
             sensor.getType() == SENSOR_TYPE_STEP_DETECTOR)) {
        // Allow access to step sensors if the application targets pre-Q, which is before the
        // requirement to hold the AR permission to access Step Counter and Step Detector events
        // was introduced.
        canAccess = true;
    } else if (IPCThreadState::self()->getCallingUid() == AID_SYSTEM) {
        // Allow access if it is requested from system.
        canAccess = true;
    } else if (hasPermissionForSensor(sensor)) {
        // Ensure that the AppOp is allowed, or that there is no necessary app op
        // for the sensor
        if (opCode >= 0) {
            const int32_t appOpMode =
                    sAppOpsManager.checkOp(opCode, IPCThreadState::self()->getCallingUid(),
                                           opPackageName);
            canAccess = (appOpMode == AppOpsManager::MODE_ALLOWED);
        } else {
            canAccess = true;
        }
    }

    if (!canAccess) {
        ALOGE("%s %s a sensor (%s) without holding %s", String8(opPackageName).c_str(),
              operation, sensor.getName().c_str(), sensor.getRequiredPermission().c_str());
    }

    return canAccess;
}

bool SensorService::hasPermissionForSensor(const Sensor& sensor) {
    bool hasPermission = false;
    const String8& requiredPermission = sensor.getRequiredPermission();

    // Runtime permissions can't use the cache as they may change.
    if (sensor.isRequiredPermissionRuntime()) {
        hasPermission = checkPermission(String16(requiredPermission),
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid(),
                /*logPermissionFailure=*/ false);
    } else {
        hasPermission = PermissionCache::checkCallingPermission(String16(requiredPermission));
    }
    return hasPermission;
}

int SensorService::getTargetSdkVersion(const String16& opPackageName) {
    // Don't query the SDK version for the ISensorManager descriptor as it
    // doesn't have one. This descriptor tends to be used for VNDK clients, but
    // can technically be set by anyone so don't give it elevated privileges.
    bool isVNDK = opPackageName.startsWith(sSensorInterfaceDescriptorPrefix) &&
                  opPackageName.contains(String16("@"));
    if (isVNDK) {
        return -1;
    }

    Mutex::Autolock packageLock(sPackageTargetVersionLock);
    int targetSdkVersion = -1;
    auto entry = sPackageTargetVersion.find(opPackageName);
    if (entry != sPackageTargetVersion.end()) {
        targetSdkVersion = entry->second;
    } else {
        sp<IBinder> binder = defaultServiceManager()->getService(String16("package_native"));
        if (binder != nullptr) {
            sp<content::pm::IPackageManagerNative> packageManager =
                    interface_cast<content::pm::IPackageManagerNative>(binder);
            if (packageManager != nullptr) {
                binder::Status status = packageManager->getTargetSdkVersionForPackage(
                        opPackageName, &targetSdkVersion);
                if (!status.isOk()) {
                    targetSdkVersion = -1;
                }
            }
        }
        sPackageTargetVersion[opPackageName] = targetSdkVersion;
    }
    return targetSdkVersion;
}

void SensorService::resetTargetSdkVersionCache(const String16& opPackageName) {
    Mutex::Autolock packageLock(sPackageTargetVersionLock);
    auto iter = sPackageTargetVersion.find(opPackageName);
    if (iter != sPackageTargetVersion.end()) {
        sPackageTargetVersion.erase(iter);
    }
}

bool SensorService::getTargetOperatingMode(const std::string &inputString, Mode *targetModeOut) {
    if (inputString == std::string("restrict")) {
      *targetModeOut = RESTRICTED;
      return true;
    }
    if (inputString == std::string("enable")) {
      *targetModeOut = NORMAL;
      return true;
    }
    if (inputString == std::string("data_injection")) {
      *targetModeOut = DATA_INJECTION;
      return true;
    }
    if (inputString == std::string("replay_data_injection")) {
      *targetModeOut = REPLAY_DATA_INJECTION;
      return true;
    }
    if (inputString == std::string("hal_bypass_replay_data_injection")) {
      *targetModeOut = HAL_BYPASS_REPLAY_DATA_INJECTION;
      return true;
    }
    return false;
}

status_t SensorService::changeOperatingMode(const Vector<String16>& args,
                                            Mode targetOperatingMode) {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    SensorDevice& dev(SensorDevice::getInstance());
    if (mCurrentOperatingMode == targetOperatingMode) {
        return NO_ERROR;
    }
    if (targetOperatingMode != NORMAL && args.size() < 2) {
        return INVALID_OPERATION;
    }
    switch (targetOperatingMode) {
      case NORMAL:
        // If currently in restricted mode, reset back to NORMAL mode else ignore.
        if (mCurrentOperatingMode == RESTRICTED) {
            mCurrentOperatingMode = NORMAL;
            // enable sensors and recover all sensor direct report
            enableAllSensorsLocked(&connLock);
        }
        if (mCurrentOperatingMode == REPLAY_DATA_INJECTION) {
            dev.disableAllSensors();
        }
        if (mCurrentOperatingMode == DATA_INJECTION ||
                mCurrentOperatingMode == REPLAY_DATA_INJECTION ||
                mCurrentOperatingMode == HAL_BYPASS_REPLAY_DATA_INJECTION) {
          resetToNormalModeLocked();
        }
        mAllowListedPackage.clear();
        return status_t(NO_ERROR);
      case RESTRICTED:
        // If in any mode other than normal, ignore.
        if (mCurrentOperatingMode != NORMAL) {
            return INVALID_OPERATION;
        }

        mCurrentOperatingMode = RESTRICTED;
        // temporarily stop all sensor direct report and disable sensors
        disableAllSensorsLocked(&connLock);
        mAllowListedPackage = String8(args[1]);
        return status_t(NO_ERROR);
      case HAL_BYPASS_REPLAY_DATA_INJECTION:
        FALLTHROUGH_INTENDED;
      case REPLAY_DATA_INJECTION:
        if (SensorServiceUtil::isUserBuild()) {
            return INVALID_OPERATION;
        }
        FALLTHROUGH_INTENDED;
      case DATA_INJECTION:
        if (mCurrentOperatingMode == NORMAL) {
            dev.disableAllSensors();
            status_t err = NO_ERROR;
            if (targetOperatingMode == HAL_BYPASS_REPLAY_DATA_INJECTION) {
                // Set SensorDevice to HAL_BYPASS_REPLAY_DATA_INJECTION_MODE. This value is not
                // injected into the HAL, nor will any events be injected into the HAL
                err = dev.setMode(HAL_BYPASS_REPLAY_DATA_INJECTION);
            } else {
                // Otherwise use DATA_INJECTION here since this value goes to the HAL and the HAL
                // doesn't have an understanding of replay vs. normal data injection.
                err = dev.setMode(DATA_INJECTION);
            }
            if (err == NO_ERROR) {
                mCurrentOperatingMode = targetOperatingMode;
            }
            if (err != NO_ERROR || targetOperatingMode == REPLAY_DATA_INJECTION) {
                // Re-enable sensors.
                dev.enableAllSensors();
            }
            mAllowListedPackage = String8(args[1]);
            return NO_ERROR;
        } else {
            // Transition to data injection mode supported only from NORMAL mode.
            return INVALID_OPERATION;
        }
        break;
      default:
        break;
    }
    return NO_ERROR;
}

void SensorService::checkWakeLockState() {
    ConnectionSafeAutolock connLock = mConnectionHolder.lock(mLock);
    checkWakeLockStateLocked(&connLock);
}

void SensorService::checkWakeLockStateLocked(ConnectionSafeAutolock* connLock) {
    if (!mWakeLockAcquired) {
        return;
    }
    bool releaseLock = true;
    for (const sp<SensorEventConnection>& connection : connLock->getActiveConnections()) {
        if (connection->needsWakeLock()) {
            releaseLock = false;
            break;
        }
    }
    if (releaseLock) {
        setWakeLockAcquiredLocked(false);
    }
}

void SensorService::sendEventsFromCache(const sp<SensorEventConnection>& connection) {
    Mutex::Autolock _l(mLock);
    connection->writeToSocketFromCache();
    if (connection->needsWakeLock()) {
        setWakeLockAcquiredLocked(true);
    }
}

bool SensorService::isAllowListedPackage(const String8& packageName) {
    return (packageName.contains(mAllowListedPackage.c_str()));
}

bool SensorService::isOperationRestrictedLocked(const String16& opPackageName) {
    if (mCurrentOperatingMode == RESTRICTED) {
        String8 package(opPackageName);
        return !isAllowListedPackage(package);
    }
    return false;
}

void SensorService::UidPolicy::registerSelf() {
    ActivityManager am;
    am.registerUidObserver(this, ActivityManager::UID_OBSERVER_GONE
            | ActivityManager::UID_OBSERVER_IDLE
            | ActivityManager::UID_OBSERVER_ACTIVE,
            ActivityManager::PROCESS_STATE_UNKNOWN,
            String16("android"));
}

void SensorService::UidPolicy::unregisterSelf() {
    ActivityManager am;
    am.unregisterUidObserver(this);
}

void SensorService::UidPolicy::onUidGone(__unused uid_t uid, __unused bool disabled) {
    onUidIdle(uid, disabled);
}

void SensorService::UidPolicy::onUidActive(uid_t uid) {
    {
        Mutex::Autolock _l(mUidLock);
        mActiveUids.insert(uid);
    }
    sp<SensorService> service = mService.promote();
    if (service != nullptr) {
        service->onUidStateChanged(uid, UID_STATE_ACTIVE);
    }
}

void SensorService::UidPolicy::onUidIdle(uid_t uid, __unused bool disabled) {
    bool deleted = false;
    {
        Mutex::Autolock _l(mUidLock);
        if (mActiveUids.erase(uid) > 0) {
            deleted = true;
        }
    }
    if (deleted) {
        sp<SensorService> service = mService.promote();
        if (service != nullptr) {
            service->onUidStateChanged(uid, UID_STATE_IDLE);
        }
    }
}

void SensorService::UidPolicy::addOverrideUid(uid_t uid, bool active) {
    updateOverrideUid(uid, active, true);
}

void SensorService::UidPolicy::removeOverrideUid(uid_t uid) {
    updateOverrideUid(uid, false, false);
}

void SensorService::UidPolicy::updateOverrideUid(uid_t uid, bool active, bool insert) {
    bool wasActive = false;
    bool isActive = false;
    {
        Mutex::Autolock _l(mUidLock);
        wasActive = isUidActiveLocked(uid);
        mOverrideUids.erase(uid);
        if (insert) {
            mOverrideUids.insert(std::pair<uid_t, bool>(uid, active));
        }
        isActive = isUidActiveLocked(uid);
    }
    if (wasActive != isActive) {
        sp<SensorService> service = mService.promote();
        if (service != nullptr) {
            service->onUidStateChanged(uid, isActive ? UID_STATE_ACTIVE : UID_STATE_IDLE);
        }
    }
}

bool SensorService::UidPolicy::isUidActive(uid_t uid) {
    // Non-app UIDs are considered always active
    if (uid < FIRST_APPLICATION_UID) {
        return true;
    }
    Mutex::Autolock _l(mUidLock);
    return isUidActiveLocked(uid);
}

bool SensorService::UidPolicy::isUidActiveLocked(uid_t uid) {
    // Non-app UIDs are considered always active
    if (uid < FIRST_APPLICATION_UID) {
        return true;
    }
    auto it = mOverrideUids.find(uid);
    if (it != mOverrideUids.end()) {
        return it->second;
    }
    return mActiveUids.find(uid) != mActiveUids.end();
}

bool SensorService::isUidActive(uid_t uid) {
    return mUidPolicy->isUidActive(uid);
}

bool SensorService::isRateCappedBasedOnPermission(const String16& opPackageName) {
    int targetSdk = getTargetSdkVersion(opPackageName);
    bool hasSamplingRatePermission = checkPermission(sAccessHighSensorSamplingRatePermission,
            IPCThreadState::self()->getCallingPid(),
            IPCThreadState::self()->getCallingUid(),
            /*logPermissionFailure=*/ false);
    if (targetSdk < __ANDROID_API_S__ ||
            (targetSdk >= __ANDROID_API_S__ && hasSamplingRatePermission)) {
        return false;
    }
    return true;
}

/**
 * Checks if a sensor should be capped according to HIGH_SAMPLING_RATE_SENSORS
 * permission.
 *
 * This needs to be kept in sync with the list defined on the Java side
 * in frameworks/base/core/java/android/hardware/SystemSensorManager.java
 */
bool SensorService::isSensorInCappedSet(int sensorType) {
    return (sensorType == SENSOR_TYPE_ACCELEROMETER
            || sensorType == SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED
            || sensorType == SENSOR_TYPE_GYROSCOPE
            || sensorType == SENSOR_TYPE_GYROSCOPE_UNCALIBRATED
            || sensorType == SENSOR_TYPE_MAGNETIC_FIELD
            || sensorType == SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED);
}

status_t SensorService::adjustSamplingPeriodBasedOnMicAndPermission(nsecs_t* requestedPeriodNs,
        const String16& opPackageName) {
    if (*requestedPeriodNs >= SENSOR_SERVICE_CAPPED_SAMPLING_PERIOD_NS) {
        return OK;
    }
    bool shouldCapBasedOnPermission = isRateCappedBasedOnPermission(opPackageName);
    if (shouldCapBasedOnPermission) {
        *requestedPeriodNs = SENSOR_SERVICE_CAPPED_SAMPLING_PERIOD_NS;
        if (isPackageDebuggable(opPackageName)) {
            return PERMISSION_DENIED;
        }
        return OK;
    }
    if (mMicSensorPrivacyPolicy->isSensorPrivacyEnabled()) {
        *requestedPeriodNs = SENSOR_SERVICE_CAPPED_SAMPLING_PERIOD_NS;
        return OK;
    }
    return OK;
}

status_t SensorService::adjustRateLevelBasedOnMicAndPermission(int* requestedRateLevel,
        const String16& opPackageName) {
    if (*requestedRateLevel <= SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL) {
        return OK;
    }
    bool shouldCapBasedOnPermission = isRateCappedBasedOnPermission(opPackageName);
    if (shouldCapBasedOnPermission) {
        *requestedRateLevel = SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL;
        if (isPackageDebuggable(opPackageName)) {
            return PERMISSION_DENIED;
        }
        return OK;
    }
    if (mMicSensorPrivacyPolicy->isSensorPrivacyEnabled()) {
        *requestedRateLevel = SENSOR_SERVICE_CAPPED_SAMPLING_RATE_LEVEL;
        return OK;
    }
    return OK;
}

void SensorService::SensorPrivacyPolicy::registerSelf() {
    AutoCallerClear acc;
    SensorPrivacyManager spm;
    mSensorPrivacyEnabled = spm.isSensorPrivacyEnabled();
    spm.addSensorPrivacyListener(this);
}

void SensorService::SensorPrivacyPolicy::unregisterSelf() {
    AutoCallerClear acc;
    SensorPrivacyManager spm;
    spm.removeSensorPrivacyListener(this);
}

bool SensorService::SensorPrivacyPolicy::isSensorPrivacyEnabled() {
    return mSensorPrivacyEnabled;
}

binder::Status SensorService::SensorPrivacyPolicy::onSensorPrivacyChanged(int toggleType __unused,
        int sensor __unused, bool enabled) {
    mSensorPrivacyEnabled = enabled;
    sp<SensorService> service = mService.promote();

    if (service != nullptr) {
        if (enabled) {
            service->disableAllSensors();
        } else {
            service->enableAllSensors();
        }
    }
    return binder::Status::ok();
}

void SensorService::MicrophonePrivacyPolicy::registerSelf() {
    AutoCallerClear acc;
    SensorPrivacyManager spm;
    mSensorPrivacyEnabled =
            spm.isToggleSensorPrivacyEnabled(
                    SensorPrivacyManager::TOGGLE_TYPE_SOFTWARE,
            SensorPrivacyManager::TOGGLE_SENSOR_MICROPHONE)
                    || spm.isToggleSensorPrivacyEnabled(
                            SensorPrivacyManager::TOGGLE_TYPE_HARDWARE,
                            SensorPrivacyManager::TOGGLE_SENSOR_MICROPHONE);
    spm.addToggleSensorPrivacyListener(this);
}

void SensorService::MicrophonePrivacyPolicy::unregisterSelf() {
    AutoCallerClear acc;
    SensorPrivacyManager spm;
    spm.removeToggleSensorPrivacyListener(this);
}

binder::Status SensorService::MicrophonePrivacyPolicy::onSensorPrivacyChanged(int toggleType __unused,
        int sensor, bool enabled) {
    if (sensor != SensorPrivacyManager::TOGGLE_SENSOR_MICROPHONE) {
        return binder::Status::ok();
    }
    mSensorPrivacyEnabled = enabled;
    sp<SensorService> service = mService.promote();

    if (service != nullptr) {
        if (enabled) {
            service->capRates();
        } else {
            service->uncapRates();
        }
    }
    return binder::Status::ok();
}

SensorService::ConnectionSafeAutolock::ConnectionSafeAutolock(
        SensorService::SensorConnectionHolder& holder, Mutex& mutex)
        : mConnectionHolder(holder), mAutolock(mutex) {}

template<typename ConnectionType>
const std::vector<sp<ConnectionType>>& SensorService::ConnectionSafeAutolock::getConnectionsHelper(
        const SortedVector<wp<ConnectionType>>& connectionList,
        std::vector<std::vector<sp<ConnectionType>>>* referenceHolder) {
    referenceHolder->emplace_back();
    std::vector<sp<ConnectionType>>& connections = referenceHolder->back();
    for (const wp<ConnectionType>& weakConnection : connectionList){
        sp<ConnectionType> connection = weakConnection.promote();
        if (connection != nullptr) {
            connections.push_back(std::move(connection));
        }
    }
    return connections;
}

const std::vector<sp<SensorService::SensorEventConnection>>&
        SensorService::ConnectionSafeAutolock::getActiveConnections() {
    return getConnectionsHelper(mConnectionHolder.mActiveConnections,
                                &mReferencedActiveConnections);
}

const std::vector<sp<SensorService::SensorDirectConnection>>&
        SensorService::ConnectionSafeAutolock::getDirectConnections() {
    return getConnectionsHelper(mConnectionHolder.mDirectConnections,
                                &mReferencedDirectConnections);
}

void SensorService::SensorConnectionHolder::addEventConnectionIfNotPresent(
        const sp<SensorService::SensorEventConnection>& connection) {
    if (mActiveConnections.indexOf(connection) < 0) {
        mActiveConnections.add(connection);
    }
}

void SensorService::SensorConnectionHolder::removeEventConnection(
        const wp<SensorService::SensorEventConnection>& connection) {
    mActiveConnections.remove(connection);
}

void SensorService::SensorConnectionHolder::addDirectConnection(
        const sp<SensorService::SensorDirectConnection>& connection) {
    mDirectConnections.add(connection);
}

void SensorService::SensorConnectionHolder::removeDirectConnection(
        const wp<SensorService::SensorDirectConnection>& connection) {
    mDirectConnections.remove(connection);
}

SensorService::ConnectionSafeAutolock SensorService::SensorConnectionHolder::lock(Mutex& mutex) {
    return ConnectionSafeAutolock(*this, mutex);
}

bool SensorService::isPackageDebuggable(const String16& opPackageName) {
    bool debugMode = false;
    sp<IBinder> binder = defaultServiceManager()->getService(String16("package_native"));
    if (binder != nullptr) {
        sp<content::pm::IPackageManagerNative> packageManager =
                interface_cast<content::pm::IPackageManagerNative>(binder);
        if (packageManager != nullptr) {
            binder::Status status = packageManager->isPackageDebuggable(
                opPackageName, &debugMode);
        }
    }
    return debugMode;
}
} // namespace android
