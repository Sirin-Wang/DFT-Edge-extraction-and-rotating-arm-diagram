#pragma once
#include "v2_highres_pipeline.h"

static Mat enhanceLowResolutionForXDoGV2(const Mat& upscaledSrc) {
    if (upscaledSrc.empty()) return Mat();
    return upscaledSrc.clone();
}
