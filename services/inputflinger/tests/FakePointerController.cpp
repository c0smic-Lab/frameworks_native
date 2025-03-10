/*
 * Copyright 2022 The Android Open Source Project
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

#include "FakePointerController.h"

#include <gtest/gtest.h>

namespace android {

void FakePointerController::setBounds(float minX, float minY, float maxX, float maxY) {
    mHaveBounds = true;
    mMinX = minX;
    mMinY = minY;
    mMaxX = maxX;
    mMaxY = maxY;
}

void FakePointerController::clearBounds() {
    mHaveBounds = false;
}

const std::map<ui::LogicalDisplayId, std::vector<int32_t>>& FakePointerController::getSpots() {
    return mSpotsByDisplay;
}

void FakePointerController::setPosition(float x, float y) {
    if (!mEnabled) return;

    mX = x;
    mY = y;
}

vec2 FakePointerController::getPosition() const {
    if (!mEnabled) {
        return {0, 0};
    }

    return {mX, mY};
}

ui::LogicalDisplayId FakePointerController::getDisplayId() const {
    if (!mEnabled || !mDisplayId) {
        return ui::LogicalDisplayId::INVALID;
    }
    return *mDisplayId;
}

void FakePointerController::setDisplayViewport(const DisplayViewport& viewport) {
    mDisplayId = viewport.displayId;
    setBounds(viewport.logicalLeft, viewport.logicalTop, viewport.logicalRight - 1,
              viewport.logicalBottom - 1);
}

void FakePointerController::updatePointerIcon(PointerIconStyle iconId) {
    ASSERT_FALSE(mIconStyle.has_value()) << "Pointer icon was set more than once";
    mIconStyle = iconId;
}

void FakePointerController::setCustomPointerIcon(const SpriteIcon& icon) {
    if (!mEnabled) return;

    ASSERT_FALSE(mCustomIconStyle.has_value()) << "Custom pointer icon was set more than once";
    mCustomIconStyle = icon.style;
}

void FakePointerController::setSkipScreenshotFlagForDisplay(ui::LogicalDisplayId displayId) {
    mDisplaysToSkipScreenshotFlagChanged = true;
    mDisplaysToSkipScreenshot.insert(displayId);
}

void FakePointerController::clearSkipScreenshotFlags() {
    mDisplaysToSkipScreenshotFlagChanged = true;
    mDisplaysToSkipScreenshot.clear();
}

void FakePointerController::assertViewportSet(ui::LogicalDisplayId displayId) {
    ASSERT_TRUE(mDisplayId);
    ASSERT_EQ(displayId, mDisplayId);
}

void FakePointerController::assertViewportNotSet() {
    ASSERT_EQ(std::nullopt, mDisplayId);
}

void FakePointerController::assertPosition(float x, float y) {
    const auto actual = getPosition();
    ASSERT_NEAR(x, actual.x, 1);
    ASSERT_NEAR(y, actual.y, 1);
}

void FakePointerController::assertSpotCount(ui::LogicalDisplayId displayId, int32_t count) {
    auto it = mSpotsByDisplay.find(displayId);
    ASSERT_TRUE(it != mSpotsByDisplay.end()) << "Spots not found for display " << displayId;
    ASSERT_EQ(static_cast<size_t>(count), it->second.size());
}

void FakePointerController::assertPointerIconSet(PointerIconStyle iconId) {
    ASSERT_TRUE(mIconStyle) << "Pointer icon style was not set";
    ASSERT_EQ(iconId, mIconStyle);
    mIconStyle.reset();
}

void FakePointerController::assertPointerIconNotSet() {
    ASSERT_EQ(std::nullopt, mIconStyle);
}

void FakePointerController::assertCustomPointerIconSet(PointerIconStyle iconId) {
    ASSERT_TRUE(mCustomIconStyle) << "Custom pointer icon was not set";
    ASSERT_EQ(iconId, mCustomIconStyle);
    mCustomIconStyle.reset();
}

void FakePointerController::assertCustomPointerIconNotSet() {
    ASSERT_EQ(std::nullopt, mCustomIconStyle);
}

void FakePointerController::assertIsSkipScreenshotFlagSet(ui::LogicalDisplayId displayId) {
    ASSERT_TRUE(mDisplaysToSkipScreenshot.find(displayId) != mDisplaysToSkipScreenshot.end());
}

void FakePointerController::assertIsSkipScreenshotFlagNotSet(ui::LogicalDisplayId displayId) {
    ASSERT_TRUE(mDisplaysToSkipScreenshot.find(displayId) == mDisplaysToSkipScreenshot.end());
}

void FakePointerController::assertSkipScreenshotFlagChanged() {
    ASSERT_TRUE(mDisplaysToSkipScreenshotFlagChanged);
    mDisplaysToSkipScreenshotFlagChanged = false;
}

void FakePointerController::assertSkipScreenshotFlagNotChanged() {
    ASSERT_FALSE(mDisplaysToSkipScreenshotFlagChanged);
}

bool FakePointerController::isPointerShown() {
    return mIsPointerShown;
}

vec2 FakePointerController::move(float deltaX, float deltaY) {
    if (!mEnabled) return {0, 0};

    mX += deltaX;
    mY += deltaY;

    const vec2 position(mX, mY);

    if (mX < mMinX) mX = mMinX;
    if (mX > mMaxX) mX = mMaxX;
    if (mY < mMinY) mY = mMinY;
    if (mY > mMaxY) mY = mMaxY;

    return {position.x - mX, position.y - mY};
}

void FakePointerController::fade(Transition) {
    if (!mEnabled) return;

    mIsPointerShown = false;
}
void FakePointerController::unfade(Transition) {
    if (!mEnabled) return;

    mIsPointerShown = true;
}

void FakePointerController::setSpots(const PointerCoords*, const uint32_t*, BitSet32 spotIdBits,
                                     ui::LogicalDisplayId displayId) {
    if (!mEnabled) return;

    std::vector<int32_t> newSpots;
    // Add spots for fingers that are down.
    for (BitSet32 idBits(spotIdBits); !idBits.isEmpty();) {
        uint32_t id = idBits.clearFirstMarkedBit();
        newSpots.push_back(id);
    }

    mSpotsByDisplay[displayId] = newSpots;
}

void FakePointerController::clearSpots() {
    if (!mEnabled) return;

    mSpotsByDisplay.clear();
}

ui::Transform FakePointerController::getDisplayTransform() const {
    return ui::Transform();
}

} // namespace android
