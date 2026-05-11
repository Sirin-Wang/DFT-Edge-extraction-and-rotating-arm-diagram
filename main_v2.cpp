#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <direct.h>
#include <io.h>

#include "v2_character_guides.h"
#include "v2_component_export.h"
#include "v2_highres_pipeline.h"
#include "v2_lowres_pipeline.h"

using namespace cv;
using namespace std;

static string pathStem(const string& path) {
    size_t slash = path.find_last_of("\\/");
    size_t start = slash == string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == string::npos || dot < start) {
        dot = path.size();
    }
    return path.substr(start, dot - start);
}

static void createDirectory(const string& dirName) {
#ifdef _WIN32
    _mkdir(dirName.c_str());
#else
    mkdir(dirName.c_str(), 0755);
#endif
}

static void clearDirectory(const string& dirName) {
#ifdef _WIN32
    string pattern = dirName + "\\*";
    _finddata_t fileInfo{};
    intptr_t handle = _findfirst(pattern.c_str(), &fileInfo);
    if (handle == -1) return;

    while (true) {
        string name = fileInfo.name;
        if (name != "." && name != "..") {
            string path = dirName + "\\" + name;
            if (fileInfo.attrib & _A_SUBDIR) {
                clearDirectory(path);
                _rmdir(path.c_str());
            } else {
                remove(path.c_str());
            }
        }

        if (_findnext(handle, &fileInfo) != 0) break;
    }

    _findclose(handle);
#endif
}

static vector<string> findArtImages() {
    vector<string> images;
#ifdef _WIN32
    vector<string> patterns = {"art*.png", "art*.jpg", "art*.jpeg"};
    for (const string& pattern : patterns) {
        _finddata_t fileInfo{};
        intptr_t handle = _findfirst(pattern.c_str(), &fileInfo);
        if (handle == -1) continue;

        while (true) {
            if ((fileInfo.attrib & _A_SUBDIR) == 0) {
                images.push_back(fileInfo.name);
            }
            if (_findnext(handle, &fileInfo) != 0) break;
        }

        _findclose(handle);
    }
#endif
    sort(images.begin(), images.end());
    images.erase(unique(images.begin(), images.end()), images.end());
    return images;
}

static bool processImageV2(const string& imagePath, const V2Options& options) {
    string outputDir = options.outputRoot + "\\" + pathStem(imagePath);
    createDirectory(options.outputRoot);
    createDirectory(outputDir);

    cout << "\n===========================================\n";
    cout << "Input: " << imagePath << "\n";
    cout << "Output: " << outputDir << "\n";
    cout << "===========================================\n";

    Mat src = imread(imagePath, IMREAD_UNCHANGED);
    if (src.empty()) {
        cout << "Error: cannot load image: " << imagePath << "\n";
        return false;
    }

    double processingScale = 1.0;
    int maxDim = max(src.cols, src.rows);
    if (maxDim > options.maxProcessingDim) {
        processingScale = options.maxProcessingDim / static_cast<double>(maxDim);
    }

    Mat resizedSrc;
    resize(src, resizedSrc, Size(), processingScale, processingScale,
           processingScale < 1.0 ? INTER_AREA : INTER_LINEAR);
    Mat resized = toBgrImage(resizedSrc);

    double effectiveInternalScale = options.internalScale;
    double lowResEnhanceScale = 1.0;
    int resizedMinDim = min(resizedSrc.cols, resizedSrc.rows);
    if (options.enhanceLowResolution && resizedMinDim > 0 && resizedMinDim < options.lowResMinDim) {
        lowResEnhanceScale = min(options.lowResMaxScale,
                                 max(1.0, options.lowResTargetMinDim / static_cast<double>(resizedMinDim)));
        effectiveInternalScale *= lowResEnhanceScale;
    }

    Mat workSrc;
    resize(resizedSrc, workSrc, Size(), effectiveInternalScale, effectiveInternalScale, INTER_CUBIC);
    if (lowResEnhanceScale > 1.0) {
        workSrc = enhanceLowResolutionForXDoGV2(workSrc);
    }

    cout << "Image loaded: " << src.cols << "x" << src.rows << "\n";
    cout << "Processing at size: " << resized.cols << "x" << resized.rows
         << " (scale=" << processingScale << ", internal=" << effectiveInternalScale << "x";
    if (lowResEnhanceScale > 1.0) {
        cout << ", low-res enhance=" << lowResEnhanceScale << "x";
    }
    cout << ")\n";

    cout << "Generating XDoG_Guide and XDoG_Support...\n";

    Mat xdogMask, strongSupport, fineDetail, filledRegions, regionOutlines, structuralSupport;
    V2CharacterGuideBundle characterGuides = buildCharacterGuideBundleV2(workSrc, Mat());
    bool tuneXdogDetails = lowResEnhanceScale <= 1.0;
    Mat lineMask = buildLineMaskV2(workSrc, characterGuides.characterGuide,
                                   &xdogMask, &strongSupport, &fineDetail,
                                   &filledRegions, &regionOutlines, &structuralSupport,
                                   tuneXdogDetails);
    lineMask = cleanBinaryMaskForContours(lineMask);

    characterGuides.characterSupport = buildCharacterLineSupportV2(toBgrImage(workSrc), lineMask,
                                                                   characterGuides.subjectMask,
                                                                   characterGuides.characterGuide);

    Mat xdogGuide, xdogSupport;
    if (!xdogMask.empty() && !characterGuides.characterGuide.empty()) {
        bitwise_and(xdogMask, characterGuides.characterGuide, xdogGuide);
    }
    if (!xdogMask.empty() && !characterGuides.characterSupport.empty()) {
        bitwise_and(xdogMask, characterGuides.characterSupport, xdogSupport);
    }

    writeMaskOutputsV2(outputDir, "XDoG_Guide", xdogGuide, resized.size());
    writeMaskOutputsV2(outputDir, "XDoG_Support", xdogSupport, resized.size());

    string componentDir = outputDir + "\\comp";
    createDirectory(componentDir);
    clearDirectory(componentDir);
    int guideComponentCount = writeConnectedComponentSVGsV2(xdogGuide, componentDir, "XDoG_Guide", resized.size());
    int supportComponentCount = writeConnectedComponentSVGsV2(xdogSupport, componentDir, "XDoG_Support", resized.size());

    cout << "  XDoG_Guide pixels: " << (xdogGuide.empty() ? 0 : countNonZero(xdogGuide)) << "\n";
    cout << "  XDoG_Support pixels: " << (xdogSupport.empty() ? 0 : countNonZero(xdogSupport)) << "\n";
    cout << "  XDoG_Guide components: " << guideComponentCount << "\n";
    cout << "  XDoG_Support components: " << supportComponentCount << "\n";

    if (options.showWindows) {
        string label = pathStem(imagePath);
        imshow(label + " V2 Original", resized);
        imshow(label + " XDoG Guide", imread(outputDir + "\\XDoG_Guide.bmp", IMREAD_GRAYSCALE));
        imshow(label + " XDoG Support", imread(outputDir + "\\XDoG_Support.bmp", IMREAD_GRAYSCALE));
    }

    return true;
}

int main(int argc, char** argv) {
    cout << "===========================================\n";
    cout << "  Anime Contour V2 - XDoG Guide/Support\n";
    cout << "===========================================\n\n";

    V2Options options;
    vector<string> imagePaths;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--no-gui" || arg == "--headless") {
            options.showWindows = false;
        } else if (arg == "--clear") {
            options.clearResults = true;
        } else if (arg == "--no-clear") {
            options.clearResults = false;
        } else if (arg == "--no-lowres-enhance") {
            options.enhanceLowResolution = false;
        } else if (arg == "--output" && i + 1 < argc) {
            options.outputRoot = argv[++i];
        } else if (arg == "--max-dim" && i + 1 < argc) {
            options.maxProcessingDim = max(320.0, atof(argv[++i]));
        } else if (arg == "--internal-scale" && i + 1 < argc) {
            options.internalScale = max(1.0, atof(argv[++i]));
        } else if (arg == "--lowres-min-dim" && i + 1 < argc) {
            options.lowResMinDim = max(120.0, atof(argv[++i]));
        } else if (arg == "--lowres-target-min-dim" && i + 1 < argc) {
            options.lowResTargetMinDim = max(options.lowResMinDim, atof(argv[++i]));
        } else if (arg == "--lowres-max-scale" && i + 1 < argc) {
            options.lowResMaxScale = max(1.0, atof(argv[++i]));
        } else {
            imagePaths.push_back(arg);
        }
    }

    createDirectory(options.outputRoot);
    if (options.clearResults) {
        clearDirectory(options.outputRoot);
    }

    if (imagePaths.empty()) {
        imagePaths = findArtImages();
    }
    if (imagePaths.empty()) {
        cout << "Error: cannot find art*.png images\n";
        return -1;
    }

    cout << "Output root: " << options.outputRoot << "\n";
    cout << "Clear all results: " << (options.clearResults ? "enabled" : "disabled") << "\n";
    cout << "Low-res upscaling: " << (options.enhanceLowResolution ? "enabled" : "disabled") << "\n";

    int successCount = 0;
    for (const auto& imagePath : imagePaths) {
        if (processImageV2(imagePath, options)) {
            ++successCount;
        }
    }

    cout << "\n===========================================\n";
    cout << "Processed " << successCount << " of " << imagePaths.size() << " images\n";
    cout << "Output folders are under " << options.outputRoot << "\\<image-name>\\\n";
    cout << "===========================================\n";

    if (options.showWindows) {
        cout << "\nPress any key to exit...\n";
        waitKey(0);
    }
    return successCount == static_cast<int>(imagePaths.size()) ? 0 : 1;
}
