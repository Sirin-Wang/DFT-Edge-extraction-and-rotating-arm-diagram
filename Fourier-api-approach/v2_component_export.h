#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <direct.h>
#endif

using namespace cv;
using namespace std;

static void ensureV2Directory(const string& dirName) {
#ifdef _WIN32
    _mkdir(dirName.c_str());
#else
    mkdir(dirName.c_str(), 0755);
#endif
}

static bool isPotraceAvailableV2() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;

#ifdef _WIN32
    int result = system("potrace -v >nul 2>nul");
#else
    int result = system("potrace -v >/dev/null 2>/dev/null");
#endif
    cached = result == 0 ? 1 : 0;
    return cached == 1;
}

static bool writePotraceSVGFromMaskV2(const Mat& mask, const string& svgPath,
                                      const Size& outputSize = Size(), bool tight = true) {
    if (mask.empty() || !isPotraceAvailableV2()) return false;

    Mat binary;
    threshold(mask, binary, 0, 255, THRESH_BINARY);
    if (countNonZero(binary) == 0) return false;

    Mat output = binary;
    if (outputSize.width > 0 && outputSize.height > 0 && output.size() != outputSize) {
        resize(binary, output, outputSize, 0, 0, INTER_NEAREST);
        threshold(output, output, 0, 255, THRESH_BINARY);
    }

    string pbmPath = svgPath + ".pbm";
    if (!imwrite(pbmPath, output)) {
        return false;
    }

    string command = "potrace \"" + pbmPath + "\" -s -i";
    if (tight) {
        command += " --tight";
    }
    command += " -o \"" + svgPath + "\"";

    int result = system(command.c_str());
    remove(pbmPath.c_str());
    return result == 0;
}

static void writeMaskOutputsV2(const string& outputDir, const string& name,
                               const Mat& mask, const Size& displaySize) {
    if (mask.empty()) return;

    Mat display;
    resize(mask, display, displaySize, 0, 0, INTER_NEAREST);
    threshold(display, display, 0, 255, THRESH_BINARY);

    string bmpPath = outputDir + "\\" + name + ".bmp";
    string svgPath = outputDir + "\\" + name + ".svg";
    imwrite(bmpPath, display);

    bool ok = writePotraceSVGFromMaskV2(mask, svgPath, displaySize, false);
    if (!ok && isPotraceAvailableV2()) {
        cout << "  Warning: potrace failed for " << name << "\n";
    }
}

static string componentFileStemV2(const string& prefix, int index) {
    ostringstream stream;
    stream << prefix << "_" << setw(4) << setfill('0') << index;
    return stream.str();
}

static int writeConnectedComponentSVGsV2(const Mat& mask, const string& componentDir,
                                         const string& prefix, const Size& displaySize) {
    if (mask.empty()) return 0;

    Mat binary;
    threshold(mask, binary, 0, 255, THRESH_BINARY);
    if (countNonZero(binary) == 0) return 0;

    ensureV2Directory(componentDir);

    Mat labels, stats, centroids;
    int labelCount = connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    int writtenCount = 0;

    for (int label = 1; label < labelCount; ++label) {
        if (stats.at<int>(label, CC_STAT_AREA) <= 0) continue;

        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);

        string svgPath = componentDir + "\\" + componentFileStemV2(prefix, writtenCount) + ".svg";
        bool ok = writePotraceSVGFromMaskV2(componentMask, svgPath, displaySize, false);
        if (ok) {
            ++writtenCount;
        } else if (isPotraceAvailableV2()) {
            cout << "  Warning: potrace failed for component " << prefix << " #" << writtenCount << "\n";
        }
    }

    return writtenCount;
}
