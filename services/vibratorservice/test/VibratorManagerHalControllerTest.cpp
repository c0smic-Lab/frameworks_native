/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *            http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "VibratorManagerHalControllerTest"

#include <cutils/atomic.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utils/Log.h>

#include <vibratorservice/VibratorManagerHalController.h>

#include "test_mocks.h"
#include "test_utils.h"

using aidl::android::hardware::vibrator::IVibrationSession;
using aidl::android::hardware::vibrator::VibrationSessionConfig;
using android::vibrator::HalController;

using namespace android;
using namespace testing;

static constexpr int MAX_ATTEMPTS = 2;
static const std::vector<int32_t> VIBRATOR_IDS = {1, 2};
static const VibrationSessionConfig SESSION_CONFIG;
static constexpr int VIBRATOR_ID = 1;

// -------------------------------------------------------------------------------------------------

class MockManagerHalWrapper : public vibrator::ManagerHalWrapper {
public:
    MOCK_METHOD(void, tryReconnect, (), (override));
    MOCK_METHOD(vibrator::HalResult<void>, ping, (), (override));
    MOCK_METHOD(vibrator::HalResult<vibrator::ManagerCapabilities>, getCapabilities, (),
                (override));
    MOCK_METHOD(vibrator::HalResult<std::vector<int32_t>>, getVibratorIds, (), (override));
    MOCK_METHOD(vibrator::HalResult<std::shared_ptr<HalController>>, getVibrator, (int32_t id),
                (override));
    MOCK_METHOD(vibrator::HalResult<void>, prepareSynced, (const std::vector<int32_t>& ids),
                (override));
    MOCK_METHOD(vibrator::HalResult<void>, triggerSynced,
                (const std::function<void()>& completionCallback), (override));
    MOCK_METHOD(vibrator::HalResult<void>, cancelSynced, (), (override));
    MOCK_METHOD(vibrator::HalResult<std::shared_ptr<IVibrationSession>>, startSession,
                (const std::vector<int32_t>& ids, const VibrationSessionConfig& s,
                 const std::function<void()>& completionCallback),
                (override));
    MOCK_METHOD(vibrator::HalResult<void>, clearSessions, (), (override));
};

// -------------------------------------------------------------------------------------------------

class VibratorManagerHalControllerTest : public Test {
public:
    void SetUp() override {
        mConnectCounter = 0;
        auto callbackScheduler = std::make_shared<vibrator::CallbackScheduler>();
        mMockHal = std::make_shared<StrictMock<MockManagerHalWrapper>>();
        auto connector = [this](std::shared_ptr<vibrator::CallbackScheduler>) {
            android_atomic_inc(&mConnectCounter);
            return mMockHal;
        };
        mController = std::make_unique<vibrator::ManagerHalController>(std::move(callbackScheduler),
                                                                       connector);
        ASSERT_NE(mController, nullptr);
    }

protected:
    int32_t mConnectCounter;
    std::shared_ptr<MockManagerHalWrapper> mMockHal;
    std::unique_ptr<vibrator::ManagerHalController> mController;

    void setHalExpectations(int32_t cardinality, vibrator::HalResult<void> voidResult,
                            vibrator::HalResult<vibrator::ManagerCapabilities> capabilitiesResult,
                            vibrator::HalResult<std::vector<int32_t>> idsResult,
                            vibrator::HalResult<std::shared_ptr<HalController>> vibratorResult,
                            vibrator::HalResult<std::shared_ptr<IVibrationSession>> sessionResult) {
        EXPECT_CALL(*mMockHal.get(), ping())
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(voidResult));
        EXPECT_CALL(*mMockHal.get(), getCapabilities())
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(capabilitiesResult));
        EXPECT_CALL(*mMockHal.get(), getVibratorIds())
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(idsResult));
        EXPECT_CALL(*mMockHal.get(), getVibrator(_))
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(vibratorResult));
        EXPECT_CALL(*mMockHal.get(), prepareSynced(_))
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(voidResult));
        EXPECT_CALL(*mMockHal.get(), triggerSynced(_))
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(voidResult));
        EXPECT_CALL(*mMockHal.get(), cancelSynced())
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(voidResult));
        EXPECT_CALL(*mMockHal.get(), startSession(_, _, _))
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(sessionResult));
        EXPECT_CALL(*mMockHal.get(), clearSessions())
                .Times(Exactly(cardinality))
                .WillRepeatedly(Return(voidResult));

        if (cardinality > 1) {
            // One reconnection for each retry.
            EXPECT_CALL(*mMockHal.get(), tryReconnect()).Times(Exactly(9 * (cardinality - 1)));
        } else {
            EXPECT_CALL(*mMockHal.get(), tryReconnect()).Times(Exactly(0));
        }
    }
};

// -------------------------------------------------------------------------------------------------

TEST_F(VibratorManagerHalControllerTest, TestInit) {
    mController->init();
    ASSERT_EQ(1, mConnectCounter);

    // Noop when wrapper was already initialized.
    mController->init();
    ASSERT_EQ(1, mConnectCounter);
}

TEST_F(VibratorManagerHalControllerTest, TestApiCallsAreForwardedToHal) {
    setHalExpectations(/* cardinality= */ 1, vibrator::HalResult<void>::ok(),
                       vibrator::HalResult<vibrator::ManagerCapabilities>::ok(
                               vibrator::ManagerCapabilities::SYNC),
                       vibrator::HalResult<std::vector<int32_t>>::ok(VIBRATOR_IDS),
                       vibrator::HalResult<std::shared_ptr<HalController>>::ok(nullptr),
                       vibrator::HalResult<std::shared_ptr<IVibrationSession>>::ok(nullptr));

    ASSERT_TRUE(mController->ping().isOk());

    auto getCapabilitiesResult = mController->getCapabilities();
    ASSERT_TRUE(getCapabilitiesResult.isOk());
    ASSERT_EQ(vibrator::ManagerCapabilities::SYNC, getCapabilitiesResult.value());

    auto getVibratorIdsResult = mController->getVibratorIds();
    ASSERT_TRUE(getVibratorIdsResult.isOk());
    ASSERT_EQ(VIBRATOR_IDS, getVibratorIdsResult.value());

    auto getVibratorResult = mController->getVibrator(VIBRATOR_ID);
    ASSERT_TRUE(getVibratorResult.isOk());
    ASSERT_EQ(nullptr, getVibratorResult.value());

    ASSERT_TRUE(mController->prepareSynced(VIBRATOR_IDS).isOk());
    ASSERT_TRUE(mController->triggerSynced([]() {}).isOk());
    ASSERT_TRUE(mController->cancelSynced().isOk());
    ASSERT_TRUE(mController->startSession(VIBRATOR_IDS, SESSION_CONFIG, []() {}).isOk());
    ASSERT_TRUE(mController->clearSessions().isOk());

    ASSERT_EQ(1, mConnectCounter);
}

TEST_F(VibratorManagerHalControllerTest, TestUnsupportedApiResultDoesNotResetHalConnection) {
    setHalExpectations(/* cardinality= */ 1, vibrator::HalResult<void>::unsupported(),
                       vibrator::HalResult<vibrator::ManagerCapabilities>::unsupported(),
                       vibrator::HalResult<std::vector<int32_t>>::unsupported(),
                       vibrator::HalResult<std::shared_ptr<HalController>>::unsupported(),
                       vibrator::HalResult<std::shared_ptr<IVibrationSession>>::unsupported());

    ASSERT_TRUE(mController->ping().isUnsupported());
    ASSERT_TRUE(mController->getCapabilities().isUnsupported());
    ASSERT_TRUE(mController->getVibratorIds().isUnsupported());
    ASSERT_TRUE(mController->getVibrator(VIBRATOR_ID).isUnsupported());
    ASSERT_TRUE(mController->prepareSynced(VIBRATOR_IDS).isUnsupported());
    ASSERT_TRUE(mController->triggerSynced([]() {}).isUnsupported());
    ASSERT_TRUE(mController->cancelSynced().isUnsupported());
    ASSERT_TRUE(mController->startSession(VIBRATOR_IDS, SESSION_CONFIG, []() {}).isUnsupported());
    ASSERT_TRUE(mController->clearSessions().isUnsupported());

    ASSERT_EQ(1, mConnectCounter);
}

TEST_F(VibratorManagerHalControllerTest, TestOperationFailedApiResultDoesNotResetHalConnection) {
    setHalExpectations(/* cardinality= */ 1, vibrator::HalResult<void>::failed("msg"),
                       vibrator::HalResult<vibrator::ManagerCapabilities>::failed("msg"),
                       vibrator::HalResult<std::vector<int32_t>>::failed("msg"),
                       vibrator::HalResult<std::shared_ptr<HalController>>::failed("msg"),
                       vibrator::HalResult<std::shared_ptr<IVibrationSession>>::failed("msg"));

    ASSERT_TRUE(mController->ping().isFailed());
    ASSERT_TRUE(mController->getCapabilities().isFailed());
    ASSERT_TRUE(mController->getVibratorIds().isFailed());
    ASSERT_TRUE(mController->getVibrator(VIBRATOR_ID).isFailed());
    ASSERT_TRUE(mController->prepareSynced(VIBRATOR_IDS).isFailed());
    ASSERT_TRUE(mController->triggerSynced([]() {}).isFailed());
    ASSERT_TRUE(mController->cancelSynced().isFailed());
    ASSERT_TRUE(mController->startSession(VIBRATOR_IDS, SESSION_CONFIG, []() {}).isFailed());
    ASSERT_TRUE(mController->clearSessions().isFailed());

    ASSERT_EQ(1, mConnectCounter);
}

TEST_F(VibratorManagerHalControllerTest, TestTransactionFailedApiResultResetsHalConnection) {
    setHalExpectations(MAX_ATTEMPTS, vibrator::HalResult<void>::transactionFailed("m"),
                       vibrator::HalResult<vibrator::ManagerCapabilities>::transactionFailed("m"),
                       vibrator::HalResult<std::vector<int32_t>>::transactionFailed("m"),
                       vibrator::HalResult<std::shared_ptr<HalController>>::transactionFailed("m"),
                       vibrator::HalResult<std::shared_ptr<IVibrationSession>>::transactionFailed(
                               "m"));

    ASSERT_TRUE(mController->ping().isFailed());
    ASSERT_TRUE(mController->getCapabilities().isFailed());
    ASSERT_TRUE(mController->getVibratorIds().isFailed());
    ASSERT_TRUE(mController->getVibrator(VIBRATOR_ID).isFailed());
    ASSERT_TRUE(mController->prepareSynced(VIBRATOR_IDS).isFailed());
    ASSERT_TRUE(mController->triggerSynced([]() {}).isFailed());
    ASSERT_TRUE(mController->cancelSynced().isFailed());
    ASSERT_TRUE(mController->startSession(VIBRATOR_IDS, SESSION_CONFIG, []() {}).isFailed());
    ASSERT_TRUE(mController->clearSessions().isFailed());

    ASSERT_EQ(1, mConnectCounter);
}

TEST_F(VibratorManagerHalControllerTest, TestFailedApiResultReturnsSuccessAfterRetries) {
    {
        InSequence seq;
        EXPECT_CALL(*mMockHal.get(), ping())
                .Times(Exactly(1))
                .WillRepeatedly(Return(vibrator::HalResult<void>::transactionFailed("message")));
        EXPECT_CALL(*mMockHal.get(), tryReconnect()).Times(Exactly(1));
        EXPECT_CALL(*mMockHal.get(), ping())
                .Times(Exactly(1))
                .WillRepeatedly(Return(vibrator::HalResult<void>::ok()));
    }

    ASSERT_TRUE(mController->ping().isOk());
    ASSERT_EQ(1, mConnectCounter);
}

TEST_F(VibratorManagerHalControllerTest, TestMultiThreadConnectsOnlyOnce) {
    ASSERT_EQ(0, mConnectCounter);

    EXPECT_CALL(*mMockHal.get(), ping())
            .Times(Exactly(10))
            .WillRepeatedly(Return(vibrator::HalResult<void>::ok()));

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.push_back(std::thread([&]() { ASSERT_TRUE(mController->ping().isOk()); }));
    }
    std::for_each(threads.begin(), threads.end(), [](std::thread& t) { t.join(); });

    // Connector was called only by the first thread to use the api.
    ASSERT_EQ(1, mConnectCounter);
}
