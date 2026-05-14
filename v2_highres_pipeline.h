#pragma once
#include "v2_common.h"
static Mat outlineFilledRegionComponents(const Mat& binary,
                                         Mat* outFilledRegions = nullptr,
                                         Mat* outRegionOutlines = nullptr) {
    if (binary.empty()) return Mat();

    Mat source;
    threshold(binary, source, 0, 255, THRESH_BINARY);

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(source, labels, stats, centroids, 8, CV_32S);

    Mat result = source.clone();
    Mat filledRegions = Mat::zeros(source.size(), CV_8UC1);
    Mat regionOutlines = Mat::zeros(source.size(), CV_8UC1);
    int minArea = max(36, static_cast<int>(source.total() / 26000));
    Mat coreKernel = getStructuringElement(MORPH_ELLIPSE, Size(3, 3));
    Mat erodeKernel = getStructuringElement(MORPH_ELLIPSE, Size(5, 5));

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int longSide = max(width, height);
        int shortSide = max(1, min(width, height));
        double fillRatio = area / static_cast<double>(max(1, width * height));
        double slenderness = longSide / static_cast<double>(shortSide);

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat core;
        erode(componentMask, core, coreKernel, Point(-1, -1), 1, BORDER_CONSTANT, Scalar(0));
        double coreRatio = countNonZero(core) / static_cast<double>(max(1, area));

        bool lineLike = shortSide <= 4
                     || fillRatio <= 0.16
                     || coreRatio <= 0.035
                     || slenderness >= 18.0;
        bool compactBlock = area >= minArea
                          && shortSide >= 7
                          && fillRatio >= 0.36
                          && coreRatio >= 0.060
                          && slenderness <= 10.0;
        bool largeFlatBlock = area >= minArea * 2
                           && shortSide >= 10
                           && fillRatio >= 0.22
                           && coreRatio >= 0.100
                           && slenderness <= 16.0;
        bool thickPatch = area >= minArea
                       && shortSide >= 10
                       && fillRatio >= 0.18
                       && coreRatio >= 0.180
                       && slenderness <= 22.0;
        if (lineLike || (!compactBlock && !largeFlatBlock && !thickPatch)) continue;

        Mat eroded;
        erode(componentMask, eroded, erodeKernel, Point(-1, -1), 1, BORDER_CONSTANT, Scalar(0));
        Mat boundary;
        bitwise_and(componentMask, ~eroded, boundary);

        result.setTo(0, componentMask);
        regionOutlines |= boundary;
        filledRegions.setTo(255, componentMask);
    }

    result |= regionOutlines;
    if (outFilledRegions) *outFilledRegions = filledRegions;
    if (outRegionOutlines) *outRegionOutlines = regionOutlines;
    return result;
}

static Mat buildStructuralSupportMask(const Mat& binary) {
    if (binary.empty()) return Mat();

    Mat source;
    threshold(binary, source, 0, 255, THRESH_BINARY);

    Mat joined;
    dilate(source, joined, getStructuringElement(MORPH_ELLIPSE, Size(13, 13)));
    morphologyEx(joined, joined, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(21, 21)));
    joined = removeSmallComponents(joined, max(80, static_cast<int>(source.total() / 9000)));

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(joined, labels, stats, centroids, 8, CV_32S);
    if (count <= 1) return Mat::zeros(source.size(), CV_8UC1);

    int maxArea = 0;
    for (int label = 1; label < count; ++label) {
        maxArea = max(maxArea, stats.at<int>(label, CC_STAT_AREA));
    }

    Mat support = Mat::zeros(source.size(), CV_8UC1);
    int areaFloor = max(static_cast<int>(source.total() / 180), static_cast<int>(maxArea * 0.14));
    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        if (area < areaFloor) continue;

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);
        support.setTo(255, componentMask);
    }

    dilate(support, support, getStructuringElement(MORPH_ELLIPSE, Size(23, 23)));
    morphologyEx(support, support, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(17, 17)));
    return support;
}

static Mat suppressBackgroundNoiseComponents(const Mat& binary, const Mat& supportAnchor,
                                             const Mat& xdogMask, Mat* outStructuralSupport = nullptr) {
    if (binary.empty()) return Mat();

    Mat source;
    threshold(binary, source, 0, 255, THRESH_BINARY);

    Mat structuralSupport = buildStructuralSupportMask(source);
    Mat anchor = (!supportAnchor.empty() && supportAnchor.size() == source.size())
        ? supportAnchor
        : Mat::zeros(source.size(), CV_8UC1);
    Mat xdog = (!xdogMask.empty() && xdogMask.size() == source.size())
        ? xdogMask
        : Mat::zeros(source.size(), CV_8UC1);

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(source, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(source.size(), CV_8UC1);

    int tinyArea = max(5, static_cast<int>(source.total() / 320000));
    int smallArea = max(20, static_cast<int>(source.total() / 95000));

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int longSide = max(width, height);
        int shortSide = max(1, min(width, height));
        double fillRatio = area / static_cast<double>(max(1, width * height));
        double slenderness = longSide / static_cast<double>(shortSide);

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat overlap;
        bitwise_and(componentMask, structuralSupport, overlap);
        double structuralRatio = countNonZero(overlap) / static_cast<double>(max(1, area));

        bitwise_and(componentMask, anchor, overlap);
        double anchorRatio = countNonZero(overlap) / static_cast<double>(max(1, area));

        bitwise_and(componentMask, xdog, overlap);
        double xdogRatio = countNonZero(overlap) / static_cast<double>(max(1, area));

        bool inStructure = structuralRatio >= 0.20;
        bool tinySpeck = area <= tinyArea && longSide <= 8;
        bool compactDot = area <= smallArea
                       && longSide <= 18
                       && slenderness < 2.4
                       && fillRatio > 0.34;
        bool weakSmall = area <= smallArea
                      && longSide <= 22
                      && anchorRatio < 0.48
                      && xdogRatio < 0.50;
        bool isolatedBackground = !inStructure;

        if (tinySpeck || compactDot || weakSmall || isolatedBackground) {
            continue;
        }

        kept.setTo(255, componentMask);
    }

    if (outStructuralSupport) *outStructuralSupport = structuralSupport;
    return kept;
}

static Mat buildLocalDensityMask(const Mat& lineCandidates, const Mat& allowedMask) {
    if (lineCandidates.empty()) return Mat();

    Mat binary;
    threshold(lineCandidates, binary, 0, 255, THRESH_BINARY);
    if (!allowedMask.empty()) {
        bitwise_and(binary, allowedMask, binary);
    }

    Mat normalized;
    binary.convertTo(normalized, CV_32F, 1.0 / 255.0);

    int window = oddKernelSize(min(lineCandidates.rows, lineCandidates.cols) / 34, 15);
    Mat density;
    boxFilter(normalized, density, CV_32F, Size(window, window), Point(-1, -1), true, BORDER_REPLICATE);

    Mat dense;
    threshold(density, dense, 0.060, 255.0, THRESH_BINARY);
    dense.convertTo(dense, CV_8U);
    if (!allowedMask.empty()) {
        dense &= allowedMask;
    }
    morphologyEx(dense, dense, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    return dense;
}

static Mat buildLocalDensityMap(const Mat& lineCandidates, const Mat& allowedMask) {
    if (lineCandidates.empty()) return Mat();

    Mat binary;
    threshold(lineCandidates, binary, 0, 255, THRESH_BINARY);
    if (!allowedMask.empty()) {
        binary &= allowedMask;
    }

    Mat normalized;
    binary.convertTo(normalized, CV_32F, 1.0 / 255.0);

    int window = oddKernelSize(min(lineCandidates.rows, lineCandidates.cols) / 32, 15);
    Mat density;
    boxFilter(normalized, density, CV_32F, Size(window, window), Point(-1, -1), true, BORDER_REPLICATE);
    return density;
}

static Mat keepComponentsWithSupportV2(const Mat& lines, const Mat& supportMask,
                                       double minSupportRatio, int minSupportHits) {
    if (lines.empty()) return Mat();
    if (supportMask.empty()) return lines.clone();

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(lines, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(lines.size(), CV_8UC1);

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        if (area <= 0) continue;

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat supportOverlap;
        bitwise_and(componentMask, supportMask, supportOverlap);
        int supportHits = countNonZero(supportOverlap);
        double supportRatio = supportHits / static_cast<double>(area);

        if (supportHits >= minSupportHits || supportRatio >= minSupportRatio) {
            kept.setTo(255, componentMask);
        }
    }

    return kept;
}

static Mat keepTopComponentsPerTileV2(const Mat& lines, const Mat& strengthMask,
                                      int tileSize, int maxPerTile, double minScore) {
    if (lines.empty()) return Mat();

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(lines, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(lines.size(), CV_8UC1);

    int tilesX = max(1, (lines.cols + tileSize - 1) / tileSize);
    int tilesY = max(1, (lines.rows + tileSize - 1) / tileSize);
    vector<int> tileUse(tilesX * tilesY, 0);

    struct Candidate {
        int label = 0;
        int tileIndex = 0;
        double score = 0.0;
    };
    vector<Candidate> candidates;
    candidates.reserve(max(0, count - 1));

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int longSide = max(width, height);
        int shortSide = max(1, min(width, height));
        double slenderness = longSide / static_cast<double>(shortSide);

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat strengthOverlap;
        bitwise_and(componentMask, strengthMask, strengthOverlap);
        int strengthHits = countNonZero(strengthOverlap);
        double strengthRatio = strengthHits / static_cast<double>(max(1, area));

        double score = longSide * 2.0 + area * 0.20 + slenderness * 7.0 + strengthRatio * 52.0;
        if (area <= 4 && strengthRatio < 0.45) score -= 45.0;
        if (shortSide > 5 && slenderness < 1.35) score -= 32.0;

        int cx = static_cast<int>(centroids.at<double>(label, 0));
        int cy = static_cast<int>(centroids.at<double>(label, 1));
        int tileX = min(tilesX - 1, max(0, cx / tileSize));
        int tileY = min(tilesY - 1, max(0, cy / tileSize));
        candidates.push_back({ label, tileY * tilesX + tileX, score });
    }

    sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.score > b.score;
    });

    for (const auto& candidate : candidates) {
        if (candidate.score < minScore) continue;
        if (tileUse[candidate.tileIndex] >= maxPerTile) continue;

        Mat componentMask;
        compare(labels, candidate.label, componentMask, CMP_EQ);
        kept.setTo(255, componentMask);
        ++tileUse[candidate.tileIndex];
    }

    return kept;
}

static Mat limitSupportExtraComponentsV2(const Mat& supportMask, const Mat& guideMask) {
    if (supportMask.empty()) return Mat();
    if (guideMask.empty() || guideMask.size() != supportMask.size()) return supportMask.clone();

    Mat support, guide;
    threshold(supportMask, support, 0, 255, THRESH_BINARY);
    threshold(guideMask, guide, 0, 255, THRESH_BINARY);

    Mat notGuide;
    bitwise_not(guide, notGuide);
    Mat extra = support & notGuide;
    if (countNonZero(extra) == 0) return support;

    Mat nearGuide;
    dilate(guide, nearGuide, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));

    Mat closedGuide;
    morphologyEx(guide, closedGuide, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    Mat flood = closedGuide.clone();
    copyMakeBorder(flood, flood, 1, 1, 1, 1, BORDER_CONSTANT, Scalar(0));
    floodFill(flood, Point(0, 0), Scalar(255));
    Mat floodInv;
    bitwise_not(flood, floodInv);
    Mat guideInterior = floodInv(Rect(1, 1, guide.cols, guide.rows));
    guideInterior.setTo(0, closedGuide);
    guideInterior = removeSmallComponents(guideInterior, max(24, static_cast<int>(guideInterior.total() / 90000)));
    {
        Mat labelsInterior, statsInterior, centroidsInterior;
        int interiorCount = connectedComponentsWithStats(guideInterior, labelsInterior,
                                                         statsInterior, centroidsInterior, 8, CV_32S);
        Mat smallInterior = Mat::zeros(guideInterior.size(), CV_8UC1);
        int minInteriorArea = max(24, static_cast<int>(guideInterior.total() / 120000));
        int maxInteriorArea = max(220, static_cast<int>(guideInterior.total() / 900));
        for (int interiorLabel = 1; interiorLabel < interiorCount; ++interiorLabel) {
            int interiorArea = statsInterior.at<int>(interiorLabel, CC_STAT_AREA);
            if (interiorArea < minInteriorArea || interiorArea > maxInteriorArea) continue;

            Mat interiorMask;
            compare(labelsInterior, interiorLabel, interiorMask, CMP_EQ);
            smallInterior.setTo(255, interiorMask);
        }
        guideInterior = smallInterior;
    }

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(extra, labels, stats, centroids, 8, CV_32S);
    Mat keptExtra = Mat::zeros(extra.size(), CV_8UC1);

    Mat densityMap = buildLocalDensityMap(extra, Mat());
    Mat highDensity;
    threshold(densityMap, highDensity, 0.040, 255.0, THRESH_BINARY);
    highDensity.convertTo(highDensity, CV_8U);
    morphologyEx(highDensity, highDensity, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));

    int tileSize = max(34, min(extra.rows, extra.cols) / 16);
    int tilesX = max(1, (extra.cols + tileSize - 1) / tileSize);
    int tilesY = max(1, (extra.rows + tileSize - 1) / tileSize);
    vector<int> tileUse(tilesX * tilesY, 0);

    struct SupportExtraCandidate {
        int label = 0;
        int tileIndex = 0;
        double score = 0.0;
        bool crowdedSmall = false;
    };
    vector<SupportExtraCandidate> candidates;
    candidates.reserve(max(0, count - 1));

    int tinyArea = max(8, static_cast<int>(extra.total() / 220000));
    int patternArea = max(18, static_cast<int>(extra.total() / 110000));

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int longSide = max(width, height);
        int shortSide = max(1, min(width, height));
        double fillRatio = area / static_cast<double>(max(1, width * height));
        double slenderness = longSide / static_cast<double>(shortSide);

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat nearOverlap;
        bitwise_and(componentMask, nearGuide, nearOverlap);
        int nearHits = countNonZero(nearOverlap);
        double nearRatio = nearHits / static_cast<double>(max(1, area));

        Mat denseOverlap;
        bitwise_and(componentMask, highDensity, denseOverlap);
        double denseRatio = countNonZero(denseOverlap) / static_cast<double>(max(1, area));

        Mat interiorOverlap;
        bitwise_and(componentMask, guideInterior, interiorOverlap);
        double interiorRatio = countNonZero(interiorOverlap) / static_cast<double>(max(1, area));

        bool tinyLoose = area <= tinyArea && longSide <= 16 && nearRatio < 0.24;
        bool patternLike = area >= patternArea
                        && longSide >= 10
                        && longSide <= 130
                        && slenderness <= 4.0
                        && fillRatio <= 0.58;
        bool strongPattern = area >= patternArea * 2
                          && longSide >= 24
                          && longSide <= 120
                          && slenderness <= 3.20
                          && fillRatio <= 0.42;
        bool usefulLine = longSide >= 18
                       && (shortSide <= 5 || slenderness >= 1.8)
                       && (nearHits >= 3 || area >= patternArea);
        bool isolatedLong = longSide >= 90 && nearRatio < 0.12 && slenderness >= 3.2;
        bool denseExtra = denseRatio >= 0.42;

        if (tinyLoose
            || isolatedLong
            || interiorRatio >= 0.55
            || (!patternLike && !usefulLine && nearRatio < 0.35)) {
            continue;
        }

        bool crowdedSmall = denseExtra && area < patternArea * 4 && longSide < 68;

        double score = area * 0.55 + longSide * 2.6 + slenderness * 5.0 + nearRatio * 46.0;
        if (patternLike) score += 38.0;
        if (crowdedSmall) score -= 68.0;
        if (tinyLoose) score -= 60.0;

        int cx = static_cast<int>(centroids.at<double>(label, 0));
        int cy = static_cast<int>(centroids.at<double>(label, 1));
        int tileX = min(tilesX - 1, max(0, cx / tileSize));
        int tileY = min(tilesY - 1, max(0, cy / tileSize));
        candidates.push_back({ label, tileY * tilesX + tileX, score, crowdedSmall });
    }

    sort(candidates.begin(), candidates.end(), [](const SupportExtraCandidate& a,
                                                  const SupportExtraCandidate& b) {
        return a.score > b.score;
    });

    for (const auto& candidate : candidates) {
        if (candidate.score < (candidate.crowdedSmall ? 62.0 : 32.0)) continue;
        int maxPerTile = candidate.crowdedSmall ? 1 : 3;
        if (tileUse[candidate.tileIndex] >= maxPerTile) continue;

        Mat componentMask;
        compare(labels, candidate.label, componentMask, CMP_EQ);
        keptExtra.setTo(255, componentMask);
        ++tileUse[candidate.tileIndex];
    }

    Mat result = guide | keptExtra;
    result &= support;
    return result;
}

static Mat removeFilledColorBlocksV2(const Mat& binary, Mat* outFilledRegions = nullptr,
                                     Mat* outRegionOutlines = nullptr) {
    if (binary.empty()) return Mat();

    Mat cleaned = outlineFilledRegionComponents(binary, outFilledRegions, outRegionOutlines);
    cleaned = removeSmallComponents(cleaned, max(3, static_cast<int>(cleaned.total() / 140000)));
    return cleaned;
}

static Mat hollowThickColorCoresV2(const Mat& binary, Mat* outFilledCores = nullptr,
                                   Mat* outCoreOutlines = nullptr) {
    if (binary.empty()) return Mat();

    Mat source;
    threshold(binary, source, 0, 255, THRESH_BINARY);

    Mat thickCore;
    erode(source, thickCore, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)),
          Point(-1, -1), 1, BORDER_CONSTANT, Scalar(0));
    morphologyEx(thickCore, thickCore, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    thickCore = removeSmallComponents(thickCore, max(18, static_cast<int>(source.total() / 52000)));
    if (countNonZero(thickCore) == 0) {
        if (outFilledCores) *outFilledCores = Mat::zeros(source.size(), CV_8UC1);
        if (outCoreOutlines) *outCoreOutlines = Mat::zeros(source.size(), CV_8UC1);
        return source;
    }

    Mat blockArea;
    dilate(thickCore, blockArea, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    blockArea &= source;

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(blockArea, labels, stats, centroids, 8, CV_32S);
    Mat filledCores = Mat::zeros(source.size(), CV_8UC1);
    Mat coreOutlines = Mat::zeros(source.size(), CV_8UC1);
    Mat result = source.clone();
    int minArea = max(24, static_cast<int>(source.total() / 70000));

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int shortSide = max(1, min(width, height));
        int longSide = max(width, height);
        if (area < minArea || shortSide < 6) continue;

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat coreOverlap;
        bitwise_and(componentMask, thickCore, coreOverlap);
        int coreHits = countNonZero(coreOverlap);
        double coreRatio = coreHits / static_cast<double>(max(1, area));
        double fillRatio = area / static_cast<double>(max(1, width * height));
        double slenderness = longSide / static_cast<double>(shortSide);
        bool thickBlock = coreHits >= max(8, area / 12)
                       && coreRatio >= 0.080
                       && fillRatio >= 0.16
                       && slenderness <= 18.0;
        if (!thickBlock) continue;

        Mat eroded;
        erode(componentMask, eroded, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)),
              Point(-1, -1), 1, BORDER_CONSTANT, Scalar(0));
        Mat boundary;
        bitwise_and(componentMask, ~eroded, boundary);

        result.setTo(0, eroded);
        filledCores |= eroded;
        coreOutlines |= boundary;
    }

    result |= coreOutlines;
    result = removeSmallComponents(result, max(3, static_cast<int>(result.total() / 140000)));
    if (outFilledCores) *outFilledCores = filledCores;
    if (outCoreOutlines) *outCoreOutlines = coreOutlines;
    return result;
}

static Mat recoverLineLikeSupportV2(const Mat& lineMask, const Mat& baseSupport,
                                    const Mat& allowedMask) {
    if (lineMask.empty()) return Mat();

    Mat candidates;
    threshold(lineMask, candidates, 0, 255, THRESH_BINARY);
    if (!allowedMask.empty() && allowedMask.size() == candidates.size()) {
        candidates &= allowedMask;
    }
    if (countNonZero(candidates) == 0) return candidates;

    Mat nearBase = Mat::zeros(candidates.size(), CV_8UC1);
    if (!baseSupport.empty() && baseSupport.size() == candidates.size() && countNonZero(baseSupport) > 0) {
        dilate(baseSupport, nearBase, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));
    }

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(candidates, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(candidates.size(), CV_8UC1);
    int minArea = max(3, static_cast<int>(candidates.total() / 150000));
    int maxArea = max(96, static_cast<int>(candidates.total() / 28));
    Mat coreKernel = getStructuringElement(MORPH_ELLIPSE, Size(3, 3));

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        if (area < minArea || area > maxArea) continue;

        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int longSide = max(width, height);
        int shortSide = max(1, min(width, height));
        double fillRatio = area / static_cast<double>(max(1, width * height));
        double slenderness = longSide / static_cast<double>(shortSide);

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat core;
        erode(componentMask, core, coreKernel, Point(-1, -1), 1, BORDER_CONSTANT, Scalar(0));
        double coreRatio = countNonZero(core) / static_cast<double>(max(1, area));

        Mat overlap;
        bitwise_and(componentMask, nearBase, overlap);
        int nearHits = countNonZero(overlap);

        double cy = centroids.at<double>(label, 1);
        bool lowerBody = cy >= candidates.rows * 0.45;
        bool lineLike = shortSide <= 6
                     || slenderness >= 2.35
                     || (fillRatio <= 0.22 && coreRatio <= 0.08);
        bool thickPatch = shortSide >= 10
                       && fillRatio >= 0.22
                       && coreRatio >= 0.08
                       && slenderness < 12.0;
        bool supported = nearHits >= 4
                      || (lowerBody && longSide >= 20 && slenderness >= 1.90 && fillRatio <= 0.34);

        if (lineLike && supported && !thickPatch) {
            kept.setTo(255, componentMask);
        }
    }

    kept = removeSmallComponents(kept, minArea);
    return kept;
}

static Mat buildXDoGLineMask(const Mat& luminance, const Mat& supportMask,
                             double sigmaSmall = 0.78, double sigmaLarge = 1.58,
                             double dogScale = 0.985, double epsilonFactor = 0.16) {
    if (luminance.empty()) return Mat();

    Mat luma;
    if (luminance.channels() == 1) {
        luma = luminance.clone();
    } else {
        cvtColor(luminance, luma, COLOR_BGR2GRAY);
    }
    if (luma.type() != CV_8UC1) {
        normalize(luma, luma, 0, 255, NORM_MINMAX);
        luma.convertTo(luma, CV_8U);
    }

    Mat support;
    if (!supportMask.empty() && supportMask.size() == luma.size() && countNonZero(supportMask) > 0) {
        support = supportMask.clone();
    } else {
        support = Mat(luma.size(), CV_8UC1, Scalar(255));
    }

    Mat blurSmall, blurLarge;
    GaussianBlur(luma, blurSmall, Size(0, 0), sigmaSmall, sigmaSmall, BORDER_REPLICATE);
    GaussianBlur(luma, blurLarge, Size(0, 0), sigmaLarge, sigmaLarge, BORDER_REPLICATE);

    Mat smallF, largeF;
    blurSmall.convertTo(smallF, CV_32F, 1.0 / 255.0);
    blurLarge.convertTo(largeF, CV_32F, 1.0 / 255.0);

    Mat dog = smallF - static_cast<float>(dogScale) * largeF;
    Scalar meanVal, stdVal;
    meanStdDev(dog, meanVal, stdVal, support);
    float epsilon = static_cast<float>(meanVal[0] - max(0.006, stdVal[0] * epsilonFactor));

    Mat maskFloat, mask;
    threshold(dog, maskFloat, epsilon, 255.0, THRESH_BINARY_INV);
    maskFloat.convertTo(mask, CV_8U);
    mask &= support;
    morphologyEx(mask, mask, MORPH_OPEN, getStructuringElement(MORPH_CROSS, Size(2, 2)));
    morphologyEx(mask, mask, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    return mask;
}

static Mat buildDetailedXDoGLineMask(const Mat& luminance, const Mat& supportMask) {
    if (luminance.empty()) return Mat();

    Mat luma;
    if (luminance.channels() == 1) {
        luma = luminance.clone();
    } else {
        cvtColor(luminance, luma, COLOR_BGR2GRAY);
    }
    if (luma.type() != CV_8UC1) {
        normalize(luma, luma, 0, 255, NORM_MINMAX);
        luma.convertTo(luma, CV_8U);
    }

    Mat support;
    if (!supportMask.empty() && supportMask.size() == luma.size() && countNonZero(supportMask) > 0) {
        support = supportMask.clone();
    } else {
        support = Mat(luma.size(), CV_8UC1, Scalar(255));
    }

    Mat equalized;
    Ptr<CLAHE> clahe = createCLAHE(1.65, Size(8, 8));
    clahe->apply(luma, equalized);

    Mat detailBase;
    addWeighted(luma, 0.68, equalized, 0.32, 0.0, detailBase);

    Mat fine = buildXDoGLineMask(detailBase, support, 0.46, 0.96, 0.965, 0.075);
    Mat mid = buildXDoGLineMask(luma, support, 0.72, 1.45, 0.980, 0.125);
    Mat coarse = buildXDoGLineMask(luma, support, 1.04, 2.08, 0.990, 0.18);

    Mat fineEdges, microEdges;
    Canny(detailBase, fineEdges, 5, 20);
    Canny(luma, microEdges, 10, 34);

    Mat blackhat;
    morphologyEx(luma, blackhat, MORPH_BLACKHAT, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    Mat darkMicro;
    threshold(blackhat, darkMicro, 3, 255, THRESH_BINARY);

    Mat detailSupport = fineEdges | microEdges | darkMicro;
    dilate(detailSupport, detailSupport, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    detailSupport &= support;

    Mat fineDetail = fine & detailSupport;
    Mat combined = mid | coarse | fineDetail;
    combined &= support;
    morphologyEx(combined, combined, MORPH_CLOSE, getStructuringElement(MORPH_CROSS, Size(2, 2)));
    combined = removeSmallComponents(combined, 1);
    return combined;
}

static Mat buildFineDetailMask(const Mat& lBlur, const Mat& luma, const Mat& supportMask) {
    if (lBlur.empty() || luma.empty()) {
        return Mat::zeros(lBlur.size(), CV_8UC1);
    }

    Mat support = supportMask.empty() ? Mat(lBlur.size(), CV_8UC1, Scalar(255)) : supportMask.clone();

    Mat upperDetailBand = Mat::zeros(lBlur.size(), CV_8UC1);
    rectangle(upperDetailBand,
              Rect(lBlur.cols * 12 / 100, lBlur.rows * 5 / 100,
                   max(1, lBlur.cols * 76 / 100), max(1, lBlur.rows * 40 / 100)),
              Scalar(255), FILLED);
    support &= upperDetailBand;

    Mat fineXdog = buildXDoGLineMask(luma, support, 0.62, 1.24, 0.975, 0.10);
    Mat coarseXdog = buildXDoGLineMask(luma, support, 0.84, 1.70, 0.985, 0.14);
    Mat localXdog = fineXdog | coarseXdog;

    Mat localEdgesSoft, localEdgesStrong;
    Canny(lBlur, localEdgesSoft, 6, 24);
    Canny(lBlur, localEdgesStrong, 14, 42);

    Mat localDark;
    adaptiveThreshold(lBlur, localDark, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY_INV, 11, 2);

    Mat localBlackhat;
    morphologyEx(lBlur, localBlackhat, MORPH_BLACKHAT, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    Mat localBlackhatMask;
    threshold(localBlackhat, localBlackhatMask, 4, 255, THRESH_BINARY);

    Mat seed = localXdog | (localEdgesSoft & support) | (localEdgesStrong & support)
             | (localDark & support) | (localBlackhatMask & support);
    seed &= support;
    morphologyEx(seed, seed, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    seed = removeSmallComponents(seed, 2);
    return seed;
}

static Mat buildUpperColorBoundarySeed(const Mat& lBlur, const Mat& aBlur, const Mat& bBlur,
                                       const Mat& saturation, const Mat& lineSupport) {
    if (lBlur.empty() || aBlur.empty() || bBlur.empty()) return Mat();

    Mat upperBand = Mat::zeros(lBlur.size(), CV_8UC1);
    rectangle(upperBand,
              Rect(lBlur.cols * 4 / 100, lBlur.rows * 3 / 100,
                   max(1, lBlur.cols * 92 / 100), max(1, lBlur.rows * 58 / 100)),
              Scalar(255), FILLED);

    Mat lSoft, aSoft, bSoft;
    Canny(lBlur, lSoft, 8, 28);
    Canny(aBlur, aSoft, 7, 24);
    Canny(bBlur, bSoft, 7, 24);

    Mat chromaEdges = aSoft | bSoft;
    Mat chromaSupport;
    dilate(chromaEdges | lSoft | lineSupport, chromaSupport, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));

    Mat saturatedSupport;
    dilate(saturation, saturatedSupport, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));

    Mat boundarySeed = ((chromaEdges & (saturatedSupport | chromaSupport)) | (lSoft & chromaSupport)) & upperBand;
    morphologyEx(boundarySeed, boundarySeed, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    boundarySeed = removeSmallComponents(boundarySeed, 2);
    return boundarySeed;
}

static Mat filterLineComponentsV2(const Mat& lines, const Mat& strongSupport,
                                  const Mat& denseMask) {
    if (lines.empty()) return Mat();

    Mat source;
    threshold(lines, source, 0, 255, THRESH_BINARY);
    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(source, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(source.size(), CV_8UC1);

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int longSide = max(width, height);
        int shortSide = max(1, min(width, height));
        double fillRatio = area / static_cast<double>(max(1, width * height));
        double slenderness = longSide / static_cast<double>(shortSide);

        if (area < 2 || longSide < 3) continue;

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        Mat supportOverlap;
        bitwise_and(componentMask, strongSupport, supportOverlap);
        int supportHits = countNonZero(supportOverlap);
        double supportRatio = supportHits / static_cast<double>(max(1, area));

        bool inDense = false;
        if (!denseMask.empty()) {
            Mat denseOverlap;
            bitwise_and(componentMask, denseMask, denseOverlap);
            inDense = countNonZero(denseOverlap) > area * 0.35;
        }

        bool lineLike = shortSide <= 4 || slenderness >= 1.45 || fillRatio <= 0.62;
        bool strongLine = supportRatio >= (inDense ? 0.24 : 0.16) || supportHits >= (inDense ? 5 : 3);
        bool longLine = longSide >= (inDense ? 24 : 14)
                     && area >= (inDense ? 7 : 4)
                     && lineLike
                     && (strongLine || supportHits >= (inDense ? 3 : 2) || supportRatio >= 0.08);
        bool compactNoise = fillRatio > 0.70
                          && slenderness < 1.75
                          && longSide < (inDense ? 42 : 28);
        bool tinyLoose = !strongLine && area <= 4 && longSide < 9;

        if (!compactNoise && !tinyLoose && ((lineLike && strongLine) || longLine)) {
            kept.setTo(255, componentMask);
        }
    }

    return kept;
}

static Mat buildLineMaskV2(const Mat& src, const Mat& detailGuide = Mat(),
                           Mat* outXdog = nullptr, Mat* outStrongSupport = nullptr,
                           Mat* outFineDetail = nullptr,
                           Mat* outFilledRegions = nullptr,
                           Mat* outRegionOutlines = nullptr,
                           Mat* outStructuralSupport = nullptr,
                           bool tuneXdogDetails = false) {
    Mat bgr = toBgrImage(src);

    Mat lab;
    cvtColor(bgr, lab, COLOR_BGR2Lab);
    vector<Mat> labChannels;
    split(lab, labChannels);

    Mat hsv;
    cvtColor(bgr, hsv, COLOR_BGR2HSV);
    vector<Mat> hsvChannels;
    split(hsv, hsvChannels);

    Mat lBlur, aBlur, bBlur;
    bilateralFilter(labChannels[0], lBlur, 7, 38.0, 7.0);
    GaussianBlur(labChannels[1], aBlur, Size(3, 3), 0.0);
    GaussianBlur(labChannels[2], bBlur, Size(3, 3), 0.0);

    Mat fullSupport(bgr.size(), CV_8UC1, Scalar(255));
    Mat guidedSupport;
    if (!detailGuide.empty() && detailGuide.size() == bgr.size() && countNonZero(detailGuide) > 0) {
        threshold(detailGuide, guidedSupport, 0, 255, THRESH_BINARY);
        morphologyEx(guidedSupport, guidedSupport, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
        dilate(guidedSupport, guidedSupport, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    }
    Mat xdogDetailSupport = guidedSupport.empty() ? fullSupport : guidedSupport;

    Mat lEdges, lStrong, aEdges, bEdges;
    Canny(lBlur, lEdges, 16, 52);
    Canny(lBlur, lStrong, 32, 96);
    Canny(aBlur, aEdges, 15, 45);
    Canny(bBlur, bEdges, 15, 45);

    Mat saturated;
    threshold(hsvChannels[1], saturated, 26, 255, THRESH_BINARY);
    Mat chromaEdges = (aEdges | bEdges) & (saturated | lStrong);

    Mat xdogRaw = tuneXdogDetails
        ? buildXDoGLineMask(labChannels[0], xdogDetailSupport, 0.74, 1.50, 0.982, 0.145)
        : buildXDoGLineMask(labChannels[0], xdogDetailSupport);
    Mat fineDetailRaw = buildFineDetailMask(lBlur, labChannels[0], xdogDetailSupport);

    Mat blackhat;
    morphologyEx(lBlur, blackhat, MORPH_BLACKHAT, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
    Mat darkLines;
    double blackhatThreshold = threshold(blackhat, darkLines, 0, 255, THRESH_BINARY | THRESH_OTSU);
    if (blackhatThreshold < 6.0) {
        threshold(blackhat, darkLines, 6, 255, THRESH_BINARY);
    }

    Mat strongDark;
    threshold(blackhat, strongDark, max(9.0, blackhatThreshold * 1.18), 255, THRESH_BINARY);

    Mat adaptiveDark;
    adaptiveThreshold(lBlur, adaptiveDark, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY_INV, 17, 5);

    Mat upperColorBoundary = buildUpperColorBoundarySeed(lBlur, aBlur, bBlur, saturated, xdogRaw | fineDetailRaw);

    Mat edgeSupport;
    dilate(lEdges | chromaEdges | xdogRaw | fineDetailRaw | upperColorBoundary,
           edgeSupport, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    Mat supportedAdaptive = adaptiveDark & edgeSupport;

    Mat filledAreaSeed = darkLines | supportedAdaptive | upperColorBoundary;
    Mat preFilledRegions, preRegionOutlines;
    Mat outlinedAreaSeed = outlineFilledRegionComponents(filledAreaSeed, &preFilledRegions, &preRegionOutlines);

    Mat sourceEdges = lEdges | chromaEdges | upperColorBoundary;
    Mat xdogStrongSupport = lEdges | lStrong | chromaEdges | strongDark | preRegionOutlines | upperColorBoundary;
    Mat xdogDenseMask = buildLocalDensityMask(xdogRaw, fullSupport);
    Mat xdogMask = filterLineComponentsV2(xdogRaw, xdogStrongSupport, xdogDenseMask);
    xdogMask = removeSmallComponents(xdogMask, max(2, static_cast<int>(xdogMask.total() / 90000)));
    Mat xdogFilledRegions, xdogRegionOutlines;
    xdogMask = removeFilledColorBlocksV2(xdogMask, &xdogFilledRegions, &xdogRegionOutlines);
    Mat xdogFilledCores, xdogCoreOutlines;
    xdogMask = hollowThickColorCoresV2(xdogMask, &xdogFilledCores, &xdogCoreOutlines);

    Mat xdogNear;
    dilate(xdogMask, xdogNear, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));

    Mat binaryCandidate = sourceEdges | outlinedAreaSeed;
    Mat binaryBridge = binaryCandidate & xdogNear;

    Mat fineSupport;
    dilate(xdogMask | lStrong | chromaEdges | upperColorBoundary,
           fineSupport, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    Mat fineDetail = (fineDetailRaw | upperColorBoundary) & fineSupport & (xdogNear | upperColorBoundary);
    fineDetail = removeSmallComponents(fineDetail, 2);

    Mat lineSeed = xdogMask | binaryBridge | fineDetail | preRegionOutlines;

    Mat supportAnchor = lStrong | chromaEdges | (strongDark & xdogNear) | fineDetail
                      | preRegionOutlines | ((sourceEdges | darkLines) & xdogNear);
    dilate(supportAnchor, supportAnchor, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));

    Mat denseMask = buildLocalDensityMask(lineSeed, fullSupport);

    Mat denseSeed = lineSeed & denseMask & supportAnchor;
    Mat sparseSeed = lineSeed & ~denseMask;

    Mat denseLines = filterLineComponentsV2(denseSeed, supportAnchor, denseMask);
    Mat sparseLines = filterLineComponentsV2(sparseSeed, supportAnchor, denseMask);
    Mat lineMask = denseLines | sparseLines | fineDetail;

    int minArea = max(3, static_cast<int>(lineMask.total() / 52000));
    lineMask = removeSmallComponents(lineMask, minArea);
    morphologyEx(lineMask, lineMask, MORPH_CLOSE, getStructuringElement(MORPH_CROSS, Size(2, 2)));
    Mat postFilledRegions, postRegionOutlines;
    lineMask = outlineFilledRegionComponents(lineMask, &postFilledRegions, &postRegionOutlines);

    Mat structuralSupport;
    lineMask = suppressBackgroundNoiseComponents(lineMask, supportAnchor | xdogMask, xdogMask, &structuralSupport);

    Mat densityMap = buildLocalDensityMap(lineMask, fullSupport);
    Mat highDensity;
    threshold(densityMap, highDensity, 0.082, 255.0, THRESH_BINARY);
    highDensity.convertTo(highDensity, CV_8U);
    morphologyEx(highDensity, highDensity, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));

    Mat strongForDense = supportAnchor | xdogMask | strongDark | lStrong | chromaEdges | postRegionOutlines | preRegionOutlines;
    Mat densePart = lineMask & highDensity;
    Mat sparsePart = lineMask & ~highDensity;
    densePart = keepComponentsWithSupportV2(densePart,
                                            strongForDense,
                                            0.30,
                                            5);
    int tileSize = max(34, min(lineMask.rows, lineMask.cols) / 12);
    densePart = keepTopComponentsPerTileV2(densePart,
                                           strongForDense,
                                           tileSize,
                                           2,
                                           36.0);
    lineMask = sparsePart | densePart;
    lineMask = removeSmallComponents(lineMask, minArea);

    if (outXdog) *outXdog = xdogMask;
    if (outStrongSupport) *outStrongSupport = supportAnchor;
    if (outFineDetail) *outFineDetail = fineDetail;
    if (outFilledRegions) *outFilledRegions = preFilledRegions | postFilledRegions | xdogFilledRegions | xdogFilledCores;
    if (outRegionOutlines) *outRegionOutlines = preRegionOutlines | postRegionOutlines | xdogRegionOutlines | xdogCoreOutlines;
    if (outStructuralSupport) *outStructuralSupport = structuralSupport;
    return lineMask;
}

static Mat cleanBinaryMaskForContours(const Mat& binary) {
    if (binary.empty()) return Mat();

    Mat cleaned;
    threshold(binary, cleaned, 0, 255, THRESH_BINARY);
    cleaned = removeSmallComponents(cleaned, max(3, static_cast<int>(cleaned.total() / 140000)));
    return cleaned;
}
