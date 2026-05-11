#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
using namespace cv;
using namespace std;
struct V2Contour {
    vector<Point> points;
    double area = 0.0;
    double arcLength = 0.0;
    double meanCurvature = 0.0;
    Rect boundingBox;
    Point centroid;
    int id = -1;
    bool closed = false;
};

struct V2Options {
    bool showWindows = true;
    bool clearResults = false;
    bool enhanceLowResolution = true;
    double maxProcessingDim = 1180.0;
    double internalScale = 1.30;
    double lowResMinDim = 640.0;
    double lowResTargetMinDim = 720.0;
    double lowResMaxScale = 1.75;
    string outputRoot = "results_v2";
};

struct V2CharacterGuideBundle {
    Mat subjectMask;
    Mat headGuide;
    Mat faceGuide;
    Mat eyeGuide;
    Mat upperBodyGuide;
    Mat lowerBodyGuide;
    Mat characterGuide;
    Mat characterSupport;
};

static Mat toBgrImage(const Mat& src) {
    if (src.channels() == 4) {
        Mat bgr;
        cvtColor(src, bgr, COLOR_BGRA2BGR);
        return bgr;
    }
    if (src.channels() == 1) {
        Mat bgr;
        cvtColor(src, bgr, COLOR_GRAY2BGR);
        return bgr;
    }
    return src.clone();
}

static int oddKernelSize(int value, int minimumValue) {
    int size = max(value, minimumValue);
    if ((size % 2) == 0) ++size;
    return size;
}

static bool isValidPixel(const Mat& img, int x, int y) {
    return x >= 0 && y >= 0 && x < img.cols && y < img.rows;
}

static Mat removeSmallComponents(const Mat& binary, int minArea) {
    if (binary.empty()) return Mat();

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(binary.size(), CV_8UC1);

    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        if (area >= max(1, minArea)) {
            Mat componentMask;
            compare(labels, label, componentMask, CMP_EQ);
            kept.setTo(255, componentMask);
        }
    }

    return kept;
}
