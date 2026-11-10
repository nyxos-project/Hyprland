#include <output/VirtualRegions.hpp>

#include <gtest/gtest.h>

using namespace Monitor::Virtual;

static void expectCovers(const std::vector<SRegionSpan>& spans, int64_t width) {
    ASSERT_FALSE(spans.empty());
    EXPECT_EQ(spans.front().x, 0);
    for (size_t i = 1; i < spans.size(); ++i) {
        EXPECT_EQ(spans[i].x, spans[i - 1].x + spans[i - 1].width);
    }
    EXPECT_EQ(spans.back().x + spans.back().width, width);
}

TEST(VirtualRegions, logicalPixelStep) {
    EXPECT_EQ(logicalPixelStep(1.0), 1);
    EXPECT_EQ(logicalPixelStep(1.25), 5);
    EXPECT_EQ(logicalPixelStep(1.5), 3);
    EXPECT_EQ(logicalPixelStep(1.6), 8);
    EXPECT_EQ(logicalPixelStep(2.0), 2);
    EXPECT_EQ(logicalPixelStep(4.0 / 3.0), 4);
    EXPECT_EQ(logicalPixelStep(0.0), 1);
    EXPECT_EQ(logicalPixelStep(-1.0), 1);
}

TEST(VirtualRegions, equalThirdsOfDualUhd) {
    const std::vector<double> ratios = {1, 1, 1};
    const auto                spans  = partition(7680, 1.0, ratios, 640);
    ASSERT_TRUE(spans.has_value()) << spans.error();
    ASSERT_EQ(spans->size(), 3);
    expectCovers(*spans, 7680);
    EXPECT_EQ((*spans)[0].width, 2560);
    EXPECT_EQ((*spans)[1].width, 2560);
    EXPECT_EQ((*spans)[2].width, 2560);
}

TEST(VirtualRegions, equalThirdsWithRemainder) {
    const std::vector<double> ratios = {1, 1, 1};
    const auto                spans  = partition(3440, 1.0, ratios, 640);
    ASSERT_TRUE(spans.has_value()) << spans.error();
    expectCovers(*spans, 3440);
    EXPECT_EQ((*spans)[0].width, 1147);
    EXPECT_EQ((*spans)[1].width, 1146);
    EXPECT_EQ((*spans)[2].width, 1147);
}

TEST(VirtualRegions, centerFocusedAtFractionalScale) {
    const auto ratios = presetRatios("center-focused", 3);
    ASSERT_TRUE(ratios.has_value());
    const auto spans = partition(7680, 1.25, *ratios, 640);
    ASSERT_TRUE(spans.has_value()) << spans.error();
    expectCovers(*spans, 7680);
    EXPECT_EQ((*spans)[0].width, 1920);
    EXPECT_EQ((*spans)[1].width, 3840);
    EXPECT_EQ((*spans)[2].width, 1920);
    for (const auto& s : *spans) {
        EXPECT_EQ(s.x % 5, 0);
        EXPECT_EQ(s.width % 5, 0);
    }
}

TEST(VirtualRegions, customRatiosSnapToScaleStep) {
    const std::vector<double> ratios = {20, 55, 25};
    const auto                spans  = partition(7680, 1.5, ratios, 640);
    ASSERT_TRUE(spans.has_value()) << spans.error();
    expectCovers(*spans, 7680);
    for (const auto& s : *spans) {
        EXPECT_EQ(s.x % 3, 0);
    }
    EXPECT_NEAR(static_cast<double>((*spans)[0].width) / 7680.0, 0.20, 0.001);
    EXPECT_NEAR(static_cast<double>((*spans)[1].width) / 7680.0, 0.55, 0.001);
}

TEST(VirtualRegions, singleRegionIsWholeOutput) {
    const std::vector<double> ratios = {1};
    const auto                spans  = partition(5120, 2.0, ratios, 640);
    ASSERT_TRUE(spans.has_value());
    ASSERT_EQ(spans->size(), 1);
    EXPECT_EQ((*spans)[0].x, 0);
    EXPECT_EQ((*spans)[0].width, 5120);
}

TEST(VirtualRegions, rejectsBadInput) {
    const std::vector<double> none;
    const std::vector<double> four     = {1, 1, 1, 1};
    const std::vector<double> negative = {1, -1};
    const std::vector<double> tiny     = {1, 20};
    const std::vector<double> two      = {1, 1};

    EXPECT_FALSE(partition(3840, 1.0, none, 640).has_value());
    EXPECT_FALSE(partition(3840, 1.0, four, 640).has_value());
    EXPECT_FALSE(partition(3840, 1.0, negative, 640).has_value());
    EXPECT_FALSE(partition(3840, 1.0, tiny, 640).has_value());
    EXPECT_FALSE(partition(0, 1.0, two, 640).has_value());
    EXPECT_FALSE(partition(3840, 0.0, two, 640).has_value());
    EXPECT_FALSE(partition(1000, 1.0, two, 640).has_value());
}

TEST(VirtualRegions, presets) {
    EXPECT_EQ(presetRatios("equal", 2)->size(), 2);
    EXPECT_FALSE(presetRatios("center", 2).has_value());
    EXPECT_FALSE(presetRatios("nope", 2).has_value());
    EXPECT_FALSE(presetRatios("equal", 0).has_value());
    EXPECT_FALSE(presetRatios("equal", 4).has_value());
}
