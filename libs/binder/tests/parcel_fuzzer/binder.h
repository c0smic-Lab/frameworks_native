/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <binder/Parcel.h>
#include <vector>

#include "parcel_fuzzer.h"

extern std::vector<ParcelRead<::android::Parcel>> BINDER_PARCEL_READ_FUNCTIONS;
extern std::vector<ParcelWrite<::android::Parcel>> BINDER_PARCEL_WRITE_FUNCTIONS;
