#pragma once
#include "v2_common.h"
#include "v2_highres_pipeline.h"
static Rect clampRectToImage(const Rect& rect, const Size& size) {
    int x0 = max(0, rect.x);
    int y0 = max(0, rect.y);
    int x1 = min(size.width, rect.x + rect.width);
    int y1 = min(size.height, rect.y + rect.height);
    if (x1 <= x0 || y1 <= y0) return Rect();
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

static Rect nonZeroBoundingBoxV2(const Mat& mask) {
    if (mask.empty()) return Rect();

    vector<Point> pixels;
    findNonZero(mask, pixels);
    if (pixels.empty()) return Rect();
    return boundingRect(pixels);
}

static Mat fillHolesV2(const Mat& binary) {
    if (binary.empty()) return Mat();

    Mat flood;
    threshold(binary, flood, 0, 255, THRESH_BINARY);
    copyMakeBorder(flood, flood, 1, 1, 1, 1, BORDER_CONSTANT, Scalar(0));
    floodFill(flood, Point(0, 0), Scalar(255));

    Mat floodInv;
    bitwise_not(flood, floodInv);
    Mat holes = floodInv(Rect(1, 1, binary.cols, binary.rows));

    Mat filled;
    bitwise_or(binary, holes, filled);
    return filled;
}

static Mat keepLargestComponentV2(const Mat& binary) {
    if (binary.empty()) return Mat();

    Mat source;
    threshold(binary, source, 0, 255, THRESH_BINARY);

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(source, labels, stats, centroids, 8, CV_32S);
    if (count <= 1) return source;

    int bestLabel = -1;
    int bestArea = 0;
    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        if (area > bestArea) {
            bestArea = area;
            bestLabel = label;
        }
    }

    Mat kept = Mat::zeros(source.size(), CV_8UC1);
    if (bestLabel >= 0) {
        compare(labels, bestLabel, kept, CMP_EQ);
    }
    return kept;
}

static Mat buildAlphaSubjectMaskV2(const Mat& src) {
    if (src.empty() || src.channels() != 4) return Mat();

    vector<Mat> channels;
    split(src, channels);

    double minAlpha = 0.0;
    double maxAlpha = 0.0;
    minMaxLoc(channels[3], &minAlpha, &maxAlpha);
    if (maxAlpha < 8.0 || minAlpha >= 250.0) return Mat();

    Mat mask;
    threshold(channels[3], mask, 12, 255, THRESH_BINARY);
    if (countNonZero(mask) < static_cast<int>(mask.total() / 300)) {
        return Mat();
    }

    morphologyEx(mask, mask, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));
    mask = removeSmallComponents(mask, max(24, static_cast<int>(mask.total() / 22000)));
    mask = keepLargestComponentV2(mask);
    mask = fillHolesV2(mask);
    morphologyEx(mask, mask, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
    return mask;
}

static bool touchesImageBorder(const Rect& rect, const Size& size, int margin) {
    return rect.x <= margin
        || rect.y <= margin
        || rect.x + rect.width >= size.width - margin
        || rect.y + rect.height >= size.height - margin;
}

static Mat buildSubjectPriorGuideV2(const Size& size) {
    Mat guide = Mat::zeros(size, CV_8UC1);
    ellipse(guide,
            Point(size.width / 2, size.height * 54 / 100),
            Size(max(1, size.width * 50 / 100), max(1, size.height * 50 / 100)),
            0, 0, 360, Scalar(255), FILLED);
    rectangle(guide,
              Rect(max(0, size.width * 6 / 100),
                   max(0, size.height * 7 / 100),
                   max(1, size.width * 88 / 100),
                   max(1, size.height * 89 / 100)),
              Scalar(255), FILLED);
    return guide;
}

static Mat keepComponentsTouchingGuideV2(const Mat& binary, const Mat& guide, int minArea) {
    if (binary.empty() || guide.empty()) return Mat::zeros(binary.size(), CV_8UC1);

    Mat source;
    threshold(binary, source, 0, 255, THRESH_BINARY);

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(source, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(source.size(), CV_8UC1);

    for (int label = 1; label < count; ++label) {
        if (stats.at<int>(label, CC_STAT_AREA) < minArea) continue;

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);
        Mat overlap;
        bitwise_and(componentMask, guide, overlap);
        if (countNonZero(overlap) > 0) {
            kept.setTo(255, componentMask);
        }
    }

    return kept;
}

static Mat extractForegroundFromBorderModelV2(const Mat& bgr) {
    Mat lab;
    cvtColor(bgr, lab, COLOR_BGR2Lab);

    int border = max(6, min(bgr.rows, bgr.cols) / 20);
    Mat subjectPrior = buildSubjectPriorGuideV2(bgr.size());
    Mat sampleMask = Mat::zeros(bgr.size(), CV_8UC1);
    sampleMask.rowRange(0, border).setTo(255);
    sampleMask.rowRange(sampleMask.rows - border, sampleMask.rows).setTo(255);
    sampleMask.colRange(0, border).setTo(255);
    sampleMask.colRange(sampleMask.cols - border, sampleMask.cols).setTo(255);

    Mat guideExclusion;
    dilate(subjectPrior, guideExclusion, getStructuringElement(MORPH_ELLIPSE, Size(21, 21)));
    sampleMask.setTo(0, guideExclusion);

    vector<Vec3f> samples;
    samples.reserve((bgr.rows * 2 + bgr.cols * 2) * border);
    for (int y = 0; y < lab.rows; ++y) {
        const Vec3b* row = lab.ptr<Vec3b>(y);
        const uchar* sampleRow = sampleMask.ptr<uchar>(y);
        for (int x = 0; x < lab.cols; ++x) {
            if (sampleRow[x] == 0) continue;
            const Vec3b& p = row[x];
            samples.emplace_back(static_cast<float>(p[0]),
                                 static_cast<float>(p[1]),
                                 static_cast<float>(p[2]));
        }
    }

    if (samples.empty()) {
        return Mat::zeros(bgr.size(), CV_8UC1);
    }

    int clusterCount = min(3, static_cast<int>(samples.size()));
    Mat sampleMat(static_cast<int>(samples.size()), 3, CV_32F);
    for (int i = 0; i < static_cast<int>(samples.size()); ++i) {
        sampleMat.at<float>(i, 0) = samples[i][0];
        sampleMat.at<float>(i, 1) = samples[i][1];
        sampleMat.at<float>(i, 2) = samples[i][2];
    }

    Mat labels, centers;
    kmeans(sampleMat, clusterCount, labels,
           TermCriteria(TermCriteria::EPS + TermCriteria::COUNT, 30, 0.5),
           3, KMEANS_PP_CENTERS, centers);

    vector<Vec3d> means(clusterCount, Vec3d(0, 0, 0));
    vector<Vec3d> variances(clusterCount, Vec3d(36.0, 36.0, 36.0));
    vector<int> counts(clusterCount, 0);
    for (int i = 0; i < sampleMat.rows; ++i) {
        int label = labels.at<int>(i, 0);
        means[label][0] += sampleMat.at<float>(i, 0);
        means[label][1] += sampleMat.at<float>(i, 1);
        means[label][2] += sampleMat.at<float>(i, 2);
        ++counts[label];
    }
    for (int k = 0; k < clusterCount; ++k) {
        if (counts[k] <= 0) continue;
        means[k] *= (1.0 / counts[k]);
    }
    for (int i = 0; i < sampleMat.rows; ++i) {
        int label = labels.at<int>(i, 0);
        Vec3d diff(sampleMat.at<float>(i, 0) - means[label][0],
                   sampleMat.at<float>(i, 1) - means[label][1],
                   sampleMat.at<float>(i, 2) - means[label][2]);
        variances[label][0] += diff[0] * diff[0];
        variances[label][1] += diff[1] * diff[1];
        variances[label][2] += diff[2] * diff[2];
    }
    for (int k = 0; k < clusterCount; ++k) {
        double denom = max(1, counts[k] - 1);
        variances[k][0] = max(36.0, variances[k][0] / denom);
        variances[k][1] = max(36.0, variances[k][1] / denom);
        variances[k][2] = max(36.0, variances[k][2] / denom);
    }

    Mat distMap(lab.size(), CV_32F, Scalar(0));
    for (int y = 0; y < lab.rows; ++y) {
        const Vec3b* labRow = lab.ptr<Vec3b>(y);
        float* distRow = distMap.ptr<float>(y);
        for (int x = 0; x < lab.cols; ++x) {
            float best = FLT_MAX;
            for (int k = 0; k < clusterCount; ++k) {
                float d0 = static_cast<float>(labRow[x][0] - means[k][0]);
                float d1 = static_cast<float>(labRow[x][1] - means[k][1]);
                float d2 = static_cast<float>(labRow[x][2] - means[k][2]);
                float dist = d0 * d0 / static_cast<float>(variances[k][0])
                           + d1 * d1 / static_cast<float>(variances[k][1])
                           + d2 * d2 / static_cast<float>(variances[k][2]);
                if (dist < best) best = dist;
            }
            distRow[x] = best;
        }
    }

    vector<Mat> labChannels;
    split(lab, labChannels);
    Mat edgeL, edgeA, edgeB, barrier;
    Canny(labChannels[0], edgeL, 30, 90);
    Canny(labChannels[1], edgeA, 20, 60);
    Canny(labChannels[2], edgeB, 20, 60);
    barrier = edgeL | edgeA | edgeB;
    morphologyEx(barrier, barrier, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    dilate(barrier, barrier, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));

    Mat strictBg, looseBg;
    threshold(distMap, strictBg, 7.5, 255, THRESH_BINARY_INV);
    threshold(distMap, looseBg, 13.5, 255, THRESH_BINARY_INV);
    strictBg.convertTo(strictBg, CV_8U);
    looseBg.convertTo(looseBg, CV_8U);

    Mat passable = looseBg.clone();
    passable.setTo(0, barrier);
    morphologyEx(passable, passable, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));

    Mat labelsPass, statsPass, centroidsPass;
    int componentCount = connectedComponentsWithStats(passable, labelsPass, statsPass, centroidsPass, 8, CV_32S);
    Mat background = Mat::zeros(passable.size(), CV_8UC1);

    for (int label = 1; label < componentCount; ++label) {
        int x = statsPass.at<int>(label, CC_STAT_LEFT);
        int y = statsPass.at<int>(label, CC_STAT_TOP);
        int w = statsPass.at<int>(label, CC_STAT_WIDTH);
        int h = statsPass.at<int>(label, CC_STAT_HEIGHT);
        bool touchesBorder = (x == 0) || (y == 0) || (x + w >= passable.cols) || (y + h >= passable.rows);
        if (!touchesBorder) continue;

        Mat componentMask;
        compare(labelsPass, label, componentMask, CMP_EQ);
        Mat sureOverlap;
        bitwise_and(componentMask, strictBg, sureOverlap);
        if (countNonZero(sureOverlap) > 0) {
            background.setTo(255, componentMask);
        }
    }

    morphologyEx(background, background, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));

    Mat foreground;
    bitwise_not(background, foreground);
    Mat borderTouchSupport = barrier | strictBg;
    dilate(borderTouchSupport, borderTouchSupport, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    Mat supportedForeground = foreground & borderTouchSupport;
    Mat unsupportedForeground = foreground & ~borderTouchSupport;
    unsupportedForeground.setTo(0, looseBg);
    foreground = supportedForeground | unsupportedForeground;

    morphologyEx(foreground, foreground, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(11, 11)));
    morphologyEx(foreground, foreground, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    foreground = fillHolesV2(foreground);

    Mat upperTrim = Mat::zeros(foreground.size(), CV_8UC1);
    int trimHeight = max(8, foreground.rows / 18);
    rectangle(upperTrim, Rect(0, 0, foreground.cols, trimHeight), Scalar(255), FILLED);
    Mat upperOutside = upperTrim & ~subjectPrior & ~barrier;
    foreground.setTo(0, upperOutside);

    Mat guided = keepComponentsTouchingGuideV2(foreground, subjectPrior,
                                               max(32, static_cast<int>(foreground.total() / 22000)));
    if (countNonZero(guided) > 0) {
        foreground = guided;
    } else {
        foreground = keepLargestComponentV2(foreground);
    }

    morphologyEx(foreground, foreground, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
    foreground = fillHolesV2(foreground);
    return foreground;
}

static Mat buildForegroundSubjectMaskV2(const Mat& src) {
    if (src.empty()) return Mat();

    Mat alphaMask = buildAlphaSubjectMaskV2(src);
    if (!alphaMask.empty()) return alphaMask;

    Mat bgr = toBgrImage(src);
    Mat foreground = extractForegroundFromBorderModelV2(bgr);
    if (countNonZero(foreground) == 0) {
        return Mat::zeros(bgr.size(), CV_8UC1);
    }

    Mat subjectPrior = buildSubjectPriorGuideV2(foreground.size());
    Mat guided = keepComponentsTouchingGuideV2(foreground, subjectPrior,
                                               max(32, static_cast<int>(foreground.total() / 22000)));
    if (countNonZero(guided) > 0) {
        foreground = guided;
    } else {
        foreground = keepLargestComponentV2(foreground);
    }

    foreground = fillHolesV2(foreground);
    morphologyEx(foreground, foreground, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    return foreground;
}

static Mat buildHeadGuideV2(const Mat& subjectMask) {
    Mat guide = Mat::zeros(subjectMask.size(), CV_8UC1);
    Rect bbox = nonZeroBoundingBoxV2(subjectMask);
    if (bbox.area() <= 0) return guide;

    Point center(bbox.x + bbox.width / 2,
                 bbox.y + bbox.height * 18 / 100);
    Size axes(max(8, bbox.width * 23 / 100),
              max(10, bbox.height * 17 / 100));
    ellipse(guide, center, axes, 0, 0, 360, Scalar(255), FILLED);

    Rect headRect(bbox.x + bbox.width * 25 / 100,
                  bbox.y,
                  max(1, bbox.width * 50 / 100),
                  max(1, bbox.height * 34 / 100));
    headRect = clampRectToImage(headRect, subjectMask.size());
    if (headRect.area() > 0) {
        rectangle(guide, headRect, Scalar(255), FILLED);
    }

    guide &= subjectMask;
    morphologyEx(guide, guide, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
    return guide;
}

static Mat buildFaceGuideV2(const Mat& bgr, const Mat& subjectMask, const Mat& headGuide) {
    Mat guide = Mat::zeros(subjectMask.size(), CV_8UC1);
    Rect headBox = nonZeroBoundingBoxV2(headGuide);
    if (headBox.area() <= 0) return guide;

    Mat lab;
    cvtColor(bgr, lab, COLOR_BGR2Lab);
    vector<Mat> labChannels;
    split(lab, labChannels);

    Mat hsv;
    cvtColor(bgr, hsv, COLOR_BGR2HSV);
    vector<Mat> hsvChannels;
    split(hsv, hsvChannels);

    Mat lowSat, bright, skinLike;
    threshold(hsvChannels[1], lowSat, 112, 255, THRESH_BINARY_INV);
    threshold(labChannels[0], bright, 104, 255, THRESH_BINARY);
    skinLike = lowSat & bright & headGuide;
    morphologyEx(skinLike, skinLike, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    morphologyEx(skinLike, skinLike, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(skinLike, labels, stats, centroids, 8, CV_32S);
    int bestLabel = -1;
    double bestScore = -1e18;

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        if (area < max(10, static_cast<int>(subjectMask.total() / 70000))) continue;

        double cx = centroids.at<double>(label, 0);
        double cy = centroids.at<double>(label, 1);
        double relX = (cx - headBox.x) / max(1.0, static_cast<double>(headBox.width));
        double relY = (cy - headBox.y) / max(1.0, static_cast<double>(headBox.height));
        double aspect = width / static_cast<double>(max(1, height));
        if (relX < 0.18 || relX > 0.82 || relY < 0.12 || relY > 0.80) continue;
        if (aspect < 0.35 || aspect > 2.8) continue;

        double centerPenalty = fabs(relX - 0.50) * 90.0 + fabs(relY - 0.45) * 60.0;
        double score = area + height * 18.0 - centerPenalty;
        if (score > bestScore) {
            bestScore = score;
            bestLabel = label;
        }
    }

    if (bestLabel >= 0) {
        Rect faceBox(stats.at<int>(bestLabel, CC_STAT_LEFT),
                     stats.at<int>(bestLabel, CC_STAT_TOP),
                     stats.at<int>(bestLabel, CC_STAT_WIDTH),
                     stats.at<int>(bestLabel, CC_STAT_HEIGHT));
        int padX = max(5, faceBox.width * 42 / 100);
        int padTop = max(4, faceBox.height * 28 / 100);
        int padBottom = max(6, faceBox.height * 48 / 100);
        Rect expanded(faceBox.x - padX,
                      faceBox.y - padTop,
                      faceBox.width + padX * 2,
                      faceBox.height + padTop + padBottom);
        expanded = clampRectToImage(expanded, subjectMask.size());
        if (expanded.area() > 0) {
            ellipse(guide,
                    Point(expanded.x + expanded.width / 2, expanded.y + expanded.height / 2),
                    Size(max(1, expanded.width / 2), max(1, expanded.height / 2)),
                    0, 0, 360, Scalar(255), FILLED);
        }
    }

    if (countNonZero(guide) == 0) {
        Point center(headBox.x + headBox.width / 2,
                     headBox.y + headBox.height * 48 / 100);
        Size axes(max(6, headBox.width * 24 / 100),
                  max(8, headBox.height * 28 / 100));
        ellipse(guide, center, axes, 0, 0, 360, Scalar(255), FILLED);
    }

    guide &= headGuide;
    guide &= subjectMask;
    morphologyEx(guide, guide, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));
    return guide;
}

static Mat buildEyeGuideV2(const Mat& bgr, const Mat& subjectMask, const Mat& faceGuide) {
    Mat guide = Mat::zeros(subjectMask.size(), CV_8UC1);
    Rect faceBox = nonZeroBoundingBoxV2(faceGuide);
    if (faceBox.area() <= 0) return guide;

    Rect eyeSearch(faceBox.x + faceBox.width * 14 / 100,
                   faceBox.y + faceBox.height * 18 / 100,
                   max(1, faceBox.width * 72 / 100),
                   max(1, faceBox.height * 30 / 100));
    eyeSearch = clampRectToImage(eyeSearch, subjectMask.size());
    if (eyeSearch.area() <= 0) return guide;

    Mat lab;
    cvtColor(bgr, lab, COLOR_BGR2Lab);
    vector<Mat> labChannels;
    split(lab, labChannels);

    Mat hsv;
    cvtColor(bgr, hsv, COLOR_BGR2HSV);
    vector<Mat> hsvChannels;
    split(hsv, hsvChannels);

    Mat lBlur;
    bilateralFilter(labChannels[0], lBlur, 5, 30.0, 5.0);

    Mat dark;
    adaptiveThreshold(lBlur, dark, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY_INV, 15, 4);
    Mat saturated;
    threshold(hsvChannels[1], saturated, 42, 255, THRESH_BINARY);
    Mat edges;
    Canny(lBlur, edges, 8, 32);

    Mat searchMask = Mat::zeros(subjectMask.size(), CV_8UC1);
    rectangle(searchMask, eyeSearch, Scalar(255), FILLED);
    searchMask &= faceGuide;

    Mat candidates = ((dark | saturated) & (edges | dark)) & searchMask;
    morphologyEx(candidates, candidates, MORPH_OPEN, getStructuringElement(MORPH_CROSS, Size(2, 2)));

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(candidates, labels, stats, centroids, 8, CV_32S);

    struct EyeSeed {
        Rect bbox;
        Point2d center;
        double score = 0.0;
    };
    vector<EyeSeed> seeds;

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        int width = stats.at<int>(label, CC_STAT_WIDTH);
        int height = stats.at<int>(label, CC_STAT_HEIGHT);
        int longSide = max(width, height);
        int shortSide = max(1, min(width, height));
        double slenderness = longSide / static_cast<double>(shortSide);
        double fillRatio = area / static_cast<double>(max(1, width * height));
        if (area < 2 || area > max(160, static_cast<int>(subjectMask.total() / 2800))) continue;
        if (longSide < 3 || width > eyeSearch.width * 52 / 100 || height > eyeSearch.height * 68 / 100) continue;
        if (slenderness < 1.10 && fillRatio > 0.80 && area > 8) continue;

        double cx = centroids.at<double>(label, 0);
        double cy = centroids.at<double>(label, 1);
        double relX = (cx - eyeSearch.x) / max(1.0, static_cast<double>(eyeSearch.width));
        double relY = (cy - eyeSearch.y) / max(1.0, static_cast<double>(eyeSearch.height));
        if (relX < 0.05 || relX > 0.95 || relY < 0.04 || relY > 0.90) continue;

        double score = area * 0.7 + longSide * 4.0 + slenderness * 7.0
                     - fabs(relY - 0.45) * 34.0;
        seeds.push_back({ Rect(stats.at<int>(label, CC_STAT_LEFT),
                               stats.at<int>(label, CC_STAT_TOP),
                               width, height),
                          Point2d(cx, cy),
                          score });
    }

    sort(seeds.begin(), seeds.end(), [](const EyeSeed& a, const EyeSeed& b) {
        return a.score > b.score;
    });

    vector<int> selected;
    double bestPairScore = -1e18;
    int bestA = -1;
    int bestB = -1;
    for (size_t i = 0; i < seeds.size(); ++i) {
        for (size_t j = i + 1; j < seeds.size(); ++j) {
            double xSep = fabs(seeds[i].center.x - seeds[j].center.x)
                        / max(1.0, static_cast<double>(eyeSearch.width));
            double yDelta = fabs(seeds[i].center.y - seeds[j].center.y)
                          / max(1.0, static_cast<double>(eyeSearch.height));
            if (xSep < 0.16 || xSep > 0.58 || yDelta > 0.32) continue;

            double midX = (seeds[i].center.x + seeds[j].center.x) * 0.5;
            double midBias = fabs(midX - (faceBox.x + faceBox.width * 0.5))
                           / max(1.0, static_cast<double>(faceBox.width));
            double pairScore = seeds[i].score + seeds[j].score
                             - fabs(xSep - 0.34) * 92.0
                             - yDelta * 70.0
                             - midBias * 80.0;
            if (pairScore > bestPairScore) {
                bestPairScore = pairScore;
                bestA = static_cast<int>(i);
                bestB = static_cast<int>(j);
            }
        }
    }

    if (bestA >= 0 && bestB >= 0) {
        selected.push_back(bestA);
        selected.push_back(bestB);
    } else {
        int keptCount = min(2, static_cast<int>(seeds.size()));
        for (int i = 0; i < keptCount; ++i) {
            if (seeds[i].score > 9.0) selected.push_back(i);
        }
    }

    int baseAxisX = max(5, faceBox.width * 11 / 100);
    int baseAxisY = max(4, faceBox.height * 7 / 100);
    for (int index : selected) {
        const EyeSeed& seed = seeds[index];
        int padX = max(5, seed.bbox.width * 70 / 100);
        int padY = max(4, seed.bbox.height * 90 / 100);
        Rect eyeRect(seed.bbox.x - padX,
                     seed.bbox.y - padY,
                     seed.bbox.width + padX * 2,
                     seed.bbox.height + padY * 2);
        eyeRect = clampRectToImage(eyeRect, subjectMask.size());
        if (eyeRect.area() <= 0) continue;

        ellipse(guide,
                Point(eyeRect.x + eyeRect.width / 2, eyeRect.y + eyeRect.height / 2),
                Size(max(baseAxisX, eyeRect.width / 2),
                     max(baseAxisY, eyeRect.height / 2)),
                0, 0, 360, Scalar(255), FILLED);
    }

    if (countNonZero(guide) == 0) {
        rectangle(guide, eyeSearch, Scalar(255), FILLED);
    }

    guide &= faceGuide;
    guide &= subjectMask;
    morphologyEx(guide, guide, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    return guide;
}

static Mat buildUpperBodyGuideV2(const Mat& subjectMask, const Mat& headGuide, const Mat& faceGuide) {
    Mat guide = Mat::zeros(subjectMask.size(), CV_8UC1);
    Rect bbox = nonZeroBoundingBoxV2(subjectMask);
    if (bbox.area() <= 0) return guide;

    int x0 = max(0, bbox.x + bbox.width * 8 / 100);
    int x1 = min(subjectMask.cols, bbox.x + bbox.width * 92 / 100);
    int y0 = max(0, bbox.y + bbox.height * 24 / 100);
    int y1 = min(subjectMask.rows, bbox.y + bbox.height * 63 / 100);
    rectangle(guide, Rect(x0, y0, max(1, x1 - x0), max(1, y1 - y0)), Scalar(255), FILLED);

    ellipse(guide,
            Point(bbox.x + bbox.width / 2, bbox.y + bbox.height * 46 / 100),
            Size(max(8, bbox.width * 36 / 100),
                 max(10, bbox.height * 24 / 100)),
            0, 0, 360, Scalar(255), FILLED);

    Mat exclude = headGuide | faceGuide;
    dilate(exclude, exclude, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
    guide.setTo(0, exclude);
    guide &= subjectMask;
    morphologyEx(guide, guide, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    morphologyEx(guide, guide, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));
    return guide;
}

static Mat buildLowerBodyGuideV2(const Mat& subjectMask, const Mat& upperBodyGuide, const Mat& headGuide) {
    Mat guide = Mat::zeros(subjectMask.size(), CV_8UC1);
    Rect bbox = nonZeroBoundingBoxV2(subjectMask);
    if (bbox.area() <= 0) return guide;

    int x0 = max(0, bbox.x + bbox.width * 8 / 100);
    int x1 = min(subjectMask.cols, bbox.x + bbox.width * 92 / 100);
    int y0 = max(0, bbox.y + bbox.height * 57 / 100);
    int y1 = min(subjectMask.rows, bbox.y + bbox.height);
    rectangle(guide, Rect(x0, y0, max(1, x1 - x0), max(1, y1 - y0)), Scalar(255), FILLED);

    Mat transition;
    dilate(upperBodyGuide, transition, getStructuringElement(MORPH_ELLIPSE, Size(15, 15)));
    guide |= (transition & subjectMask);

    Mat exclude = headGuide.clone();
    dilate(exclude, exclude, getStructuringElement(MORPH_ELLIPSE, Size(17, 17)));
    guide.setTo(0, exclude);
    guide &= subjectMask;
    morphologyEx(guide, guide, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    morphologyEx(guide, guide, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
    return guide;
}

static Mat buildCharacterLineSupportV2(const Mat& bgr, const Mat& lineMask, const Mat& subjectMask,
                                       const Mat& characterGuide) {
    Mat support = Mat::zeros(subjectMask.size(), CV_8UC1);
    if (bgr.empty() || subjectMask.empty() || characterGuide.empty()) return support;

    Mat lab;
    cvtColor(bgr, lab, COLOR_BGR2Lab);
    vector<Mat> labChannels;
    split(lab, labChannels);

    Mat hsv;
    cvtColor(bgr, hsv, COLOR_BGR2HSV);
    vector<Mat> hsvChannels;
    split(hsv, hsvChannels);

    Mat lBlur, aBlur, bBlur;
    bilateralFilter(labChannels[0], lBlur, 5, 32.0, 5.0);
    GaussianBlur(labChannels[1], aBlur, Size(3, 3), 0.0);
    GaussianBlur(labChannels[2], bBlur, Size(3, 3), 0.0);

    Mat lEdges, aEdges, bEdges;
    Canny(lBlur, lEdges, 12, 44);
    Canny(aBlur, aEdges, 10, 34);
    Canny(bBlur, bEdges, 10, 34);

    Mat dark;
    adaptiveThreshold(lBlur, dark, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY_INV, 17, 5);

    Mat blackhat;
    morphologyEx(lBlur, blackhat, MORPH_BLACKHAT, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));
    Mat strongDark;
    threshold(blackhat, strongDark, 7, 255, THRESH_BINARY);

    Mat saturated;
    threshold(hsvChannels[1], saturated, 34, 255, THRESH_BINARY);

    support = (lEdges | aEdges | bEdges | (dark & saturated) | strongDark);
    if (!lineMask.empty() && lineMask.size() == support.size()) {
        support |= lineMask;
    }
    dilate(support, support, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    support &= subjectMask;
    support &= characterGuide;
    morphologyEx(support, support, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    support = removeSmallComponents(support, max(2, static_cast<int>(support.total() / 140000)));
    return support;
}

static V2CharacterGuideBundle buildCharacterGuideBundleV2(const Mat& src, const Mat& lineMask) {
    V2CharacterGuideBundle guides;
    if (src.empty()) return guides;

    Mat bgr = toBgrImage(src);
    guides.subjectMask = buildForegroundSubjectMaskV2(src);
    if (guides.subjectMask.empty() || countNonZero(guides.subjectMask) == 0) {
        guides.subjectMask = Mat(src.size(), CV_8UC1, Scalar(255));
    }

    guides.headGuide = buildHeadGuideV2(guides.subjectMask);
    guides.faceGuide = buildFaceGuideV2(bgr, guides.subjectMask, guides.headGuide);
    guides.eyeGuide = buildEyeGuideV2(bgr, guides.subjectMask, guides.faceGuide);
    guides.upperBodyGuide = buildUpperBodyGuideV2(guides.subjectMask, guides.headGuide, guides.faceGuide);
    guides.lowerBodyGuide = buildLowerBodyGuideV2(guides.subjectMask, guides.upperBodyGuide, guides.headGuide);
    guides.characterGuide = guides.subjectMask.clone();
    morphologyEx(guides.characterGuide, guides.characterGuide,
                 MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(11, 11)));
    dilate(guides.characterGuide, guides.characterGuide,
           getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    guides.characterSupport = buildCharacterLineSupportV2(bgr, lineMask, guides.subjectMask, guides.characterGuide);
    return guides;
}
