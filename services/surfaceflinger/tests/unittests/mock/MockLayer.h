/*
 * Copyright 2019 The Android Open Source Project
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

#pragma once

#include <gmock/gmock.h>
#include <optional>

#include "Layer.h"

namespace android::mock {

class MockLayer : public Layer {
public:
    MockLayer(SurfaceFlinger* flinger, std::string name)
          : Layer(LayerCreationArgs(flinger, nullptr, std::move(name), 0, {})) {}

    MockLayer(SurfaceFlinger* flinger, std::string name, std::optional<uint32_t> id)
          : Layer(LayerCreationArgs(flinger, nullptr, std::move(name), 0, {}, id)) {}

    MockLayer(SurfaceFlinger* flinger, std::optional<uint32_t> id)
          : Layer(LayerCreationArgs(flinger, nullptr, "TestLayer", 0, {}, id)) {}

    explicit MockLayer(SurfaceFlinger* flinger) : MockLayer(flinger, "TestLayer") {}

    MOCK_METHOD0(getFrameSelectionPriority, int32_t());
    MOCK_METHOD0(createClone, sp<Layer>());
    MOCK_CONST_METHOD0(getFrameRateForLayerTree, FrameRate());
    MOCK_CONST_METHOD0(getDefaultFrameRateCompatibility, scheduler::FrameRateCompatibility());
    MOCK_CONST_METHOD0(getOwnerUid, uid_t());
};

} // namespace android::mock
