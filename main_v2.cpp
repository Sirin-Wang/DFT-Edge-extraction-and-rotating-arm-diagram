#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
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

static Mat buildLowerBodySoftSupportV2(const Mat& bgr, const Mat& lowerBodyGuide,
                                       const Mat& subjectMask, const Mat& anchorLines) {
    if (bgr.empty() || lowerBodyGuide.empty() || subjectMask.empty()) return Mat();

    Mat lowerMask;
    threshold(lowerBodyGuide, lowerMask, 0, 255, THRESH_BINARY);
    if (lowerMask.empty() || countNonZero(lowerMask) == 0) return Mat::zeros(bgr.size(), CV_8UC1);
    if (lowerMask.size() != bgr.size()) {
        resize(lowerMask, lowerMask, bgr.size(), 0, 0, INTER_NEAREST);
    }

    Mat subject;
    threshold(subjectMask, subject, 0, 255, THRESH_BINARY);
    if (subject.size() != bgr.size()) {
        resize(subject, subject, bgr.size(), 0, 0, INTER_NEAREST);
    }
    lowerMask &= subject;

    Mat lab, hsv;
    cvtColor(bgr, lab, COLOR_BGR2Lab);
    cvtColor(bgr, hsv, COLOR_BGR2HSV);

    vector<Mat> labChannels, hsvChannels;
    split(lab, labChannels);
    split(hsv, hsvChannels);

    Mat lBlur;
    bilateralFilter(labChannels[0], lBlur, 5, 24.0, 5.0);

    Mat bright, lowSat, legTone;
    threshold(labChannels[0], bright, 170, 255, THRESH_BINARY);
    threshold(hsvChannels[1], lowSat, 58, 255, THRESH_BINARY_INV);
    legTone = bright & lowSat & lowerMask;
    morphologyEx(legTone, legTone, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    morphologyEx(legTone, legTone, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(7, 7)));
    {
        Mat labels, stats, centroids;
        int count = connectedComponentsWithStats(legTone, labels, stats, centroids, 8, CV_32S);
        Mat keptTone = Mat::zeros(legTone.size(), CV_8UC1);
        int minLegArea = max(48, static_cast<int>(legTone.total() / 9000));
        int minLegBottom = static_cast<int>(legTone.rows * 0.76);
        int maxLegWidth = max(18, static_cast<int>(legTone.cols * 0.18));
        for (int label = 1; label < count; ++label) {
            int area = stats.at<int>(label, CC_STAT_AREA);
            if (area < minLegArea) continue;

            int x = stats.at<int>(label, CC_STAT_LEFT);
            int y = stats.at<int>(label, CC_STAT_TOP);
            int width = stats.at<int>(label, CC_STAT_WIDTH);
            int height = stats.at<int>(label, CC_STAT_HEIGHT);
            int bottom = y + height;
            double cx = centroids.at<double>(label, 0);
            bool likelyLeg = bottom >= minLegBottom
                          && height >= legTone.rows * 0.16
                          && width <= maxLegWidth
                          && height >= width * 1.80
                          && cx >= legTone.cols * 0.20
                          && cx <= legTone.cols * 0.80
                          && x > 0;
            if (!likelyLeg) continue;

            Mat componentMask;
            compare(labels, label, componentMask, CMP_EQ);
            keptTone.setTo(255, componentMask);
        }
        if (countNonZero(keptTone) > 0) {
            legTone = keptTone;
        }
    }
    if (countNonZero(legTone) == 0) return Mat::zeros(bgr.size(), CV_8UC1);

    Mat toneBand;
    dilate(legTone, toneBand, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    toneBand &= lowerMask;

    Mat softEdges, verySoftEdges;
    Canny(lBlur, softEdges, 4, 18);
    Canny(lBlur, verySoftEdges, 7, 26);

    Mat softShadows;
    adaptiveThreshold(lBlur, softShadows, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY_INV, 31, 3);
    softShadows &= toneBand;
    morphologyEx(softShadows, softShadows, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, Size(3, 3)));
    softShadows = removeSmallComponents(softShadows, max(8, static_cast<int>(softShadows.total() / 120000)));
    Mat softShadowOutlines = outlineFilledRegionComponents(softShadows);

    Mat gradX, gradY, gradMag;
    Sobel(lBlur, gradX, CV_16S, 1, 0, 3);
    Sobel(lBlur, gradY, CV_16S, 0, 1, 3);
    Mat absX, absY;
    convertScaleAbs(gradX, absX);
    convertScaleAbs(gradY, absY);
    addWeighted(absX, 0.5, absY, 0.5, 0.0, gradMag);

    Mat gentleGradient;
    threshold(gradMag, gentleGradient, 10, 255, THRESH_BINARY);
    gentleGradient &= toneBand;

    Mat candidates = (softEdges | verySoftEdges | gentleGradient | softShadowOutlines) & toneBand;
    morphologyEx(candidates, candidates, MORPH_CLOSE, getStructuringElement(MORPH_CROSS, Size(2, 2)));

    Mat nearAnchor = Mat::zeros(candidates.size(), CV_8UC1);
    if (!anchorLines.empty() && anchorLines.size() == candidates.size() && countNonZero(anchorLines) > 0) {
        dilate(anchorLines, nearAnchor, getStructuringElement(MORPH_ELLIPSE, Size(9, 9)));
    } else {
        nearAnchor = lowerMask.clone();
    }

    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(candidates, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(candidates.size(), CV_8UC1);
    int minArea = max(3, static_cast<int>(candidates.total() / 220000));
    int maxArea = max(80, static_cast<int>(candidates.total() / 260));

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

        Mat anchorOverlap;
        bitwise_and(componentMask, nearAnchor, anchorOverlap);
        int anchorHits = countNonZero(anchorOverlap);

        bool lineLike = shortSide <= 5 || slenderness >= 1.35 || fillRatio <= 0.46;
        bool supported = anchorHits >= 2 || longSide >= 8 || (area >= 10 && fillRatio <= 0.36);
        bool compactPatch = fillRatio > 0.68 && slenderness < 1.65 && longSide < 24;
        if (lineLike && supported && !compactPatch) {
            kept.setTo(255, componentMask);
        }
    }

    kept = removeSmallComponents(kept, minArea);
    return kept;
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

struct IntermediateImageV2 {
    string name;
    Mat image;
};

static string intermediateNameV2(int index, const string& label) {
    ostringstream stream;
    stream << setw(2) << setfill('0') << index << "_" << label;
    return stream.str();
}

static Mat resizeForDisplayV2(const Mat& image, const Size& displaySize, bool nearest) {
    if (image.empty()) return Mat();

    Mat output;
    int interpolation = nearest ? INTER_NEAREST : INTER_AREA;
    if (image.size() == displaySize) {
        output = image.clone();
    } else {
        resize(image, output, displaySize, 0, 0, interpolation);
    }
    return output;
}

static Mat normalizeMaskForDisplayV2(const Mat& mask, const Size& displaySize) {
    if (mask.empty()) return Mat();

    Mat gray;
    if (mask.channels() == 1) {
        gray = mask.clone();
    } else {
        cvtColor(mask, gray, COLOR_BGR2GRAY);
    }
    if (gray.type() != CV_8UC1) {
        normalize(gray, gray, 0, 255, NORM_MINMAX);
        gray.convertTo(gray, CV_8U);
    }

    Mat display = resizeForDisplayV2(gray, displaySize, true);
    threshold(display, display, 0, 255, THRESH_BINARY);
    return display;
}

static Mat normalizeImageForDisplayV2(const Mat& image, const Size& displaySize) {
    if (image.empty()) return Mat();

    Mat bgr = toBgrImage(image);
    if (bgr.type() != CV_8UC3) {
        normalize(bgr, bgr, 0, 255, NORM_MINMAX);
        bgr.convertTo(bgr, CV_8U);
    }
    return resizeForDisplayV2(bgr, displaySize, false);
}

static Mat makeOverlayPreviewV2(const Mat& baseImage, const Mat& mask, const Size& displaySize,
                                const Scalar& color) {
    if (baseImage.empty() || mask.empty()) return Mat();

    Mat base = normalizeImageForDisplayV2(baseImage, displaySize);
    Mat displayMask = normalizeMaskForDisplayV2(mask, displaySize);
    if (base.empty() || displayMask.empty()) return Mat();

    Mat colorLayer(base.size(), base.type(), color);
    Mat blended;
    addWeighted(base, 0.72, colorLayer, 0.28, 0.0, blended);
    blended.copyTo(base, displayMask);
    return base;
}

static void addIntermediateImageV2(vector<IntermediateImageV2>& images,
                                   const string& label, const Mat& image) {
    if (image.empty()) return;
    images.push_back({label, image.clone()});
}

static Mat makeContactSheetV2(const vector<IntermediateImageV2>& images) {
    if (images.empty()) return Mat();

    const int thumbW = 280;
    const int thumbH = 280;
    const int labelH = 30;
    const int pad = 12;
    const int cols = min(4, max(1, static_cast<int>(images.size())));
    const int rows = (static_cast<int>(images.size()) + cols - 1) / cols;

    Mat sheet(rows * (thumbH + labelH + pad) + pad,
              cols * (thumbW + pad) + pad,
              CV_8UC3,
              Scalar(246, 246, 246));

    for (int i = 0; i < static_cast<int>(images.size()); ++i) {
        int col = i % cols;
        int row = i / cols;
        int x = pad + col * (thumbW + pad);
        int y = pad + row * (thumbH + labelH + pad);

        Mat tile = normalizeImageForDisplayV2(images[i].image, Size(thumbW, thumbH));
        if (tile.empty()) continue;

        Rect target(x, y + labelH, thumbW, thumbH);
        tile.copyTo(sheet(target));
        rectangle(sheet, target, Scalar(210, 210, 210), 1);
        putText(sheet, images[i].name, Point(x, y + 20), FONT_HERSHEY_SIMPLEX,
                0.48, Scalar(40, 40, 40), 1, LINE_AA);
    }

    return sheet;
}

static void writeIntermediateImagesV2(const string& outputDir,
                                      const vector<IntermediateImageV2>& images) {
    if (images.empty()) return;

    string intermediateDir = outputDir + "\\intermediate";
    createDirectory(intermediateDir);
    clearDirectory(intermediateDir);

    for (int i = 0; i < static_cast<int>(images.size()); ++i) {
        string path = intermediateDir + "\\" + intermediateNameV2(i, images[i].name) + ".png";
        imwrite(path, images[i].image);
    }

    Mat contactSheet = makeContactSheetV2(images);
    if (!contactSheet.empty()) {
        imwrite(outputDir + "\\art_intermediate_case.png", contactSheet);
    }
}

static void deleteIntermediateImagesV2(const string& outputDir) {
    string intermediateDir = outputDir + "\\intermediate";
    clearDirectory(intermediateDir);
#ifdef _WIN32
    _rmdir(intermediateDir.c_str());
#endif
    remove((outputDir + "\\art_intermediate_case.png").c_str());
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

    Mat xdogGuide, xdogSupport, lowerBodySoftSupport;
    if (!xdogMask.empty() && !characterGuides.characterGuide.empty()) {
        bitwise_and(xdogMask, characterGuides.characterGuide, xdogGuide);
        xdogGuide = removeFilledColorBlocksV2(xdogGuide);
        xdogGuide = hollowThickColorCoresV2(xdogGuide);
        xdogGuide = cleanBinaryMaskForContours(xdogGuide);
    }
    if (!xdogMask.empty() && !characterGuides.characterSupport.empty()) {
        bitwise_and(xdogMask, characterGuides.characterSupport, xdogSupport);
        xdogSupport |= recoverLineLikeSupportV2(lineMask, xdogSupport, characterGuides.characterSupport);
        if (!xdogGuide.empty() && xdogGuide.size() == xdogSupport.size()) {
            xdogSupport |= xdogGuide;
        }
        lowerBodySoftSupport = buildLowerBodySoftSupportV2(toBgrImage(workSrc),
                                                           characterGuides.lowerBodyGuide,
                                                           characterGuides.subjectMask,
                                                           xdogSupport | lineMask);
        if (!lowerBodySoftSupport.empty()) {
            xdogSupport |= lowerBodySoftSupport;
        }
        xdogSupport = removeFilledColorBlocksV2(xdogSupport);
        xdogSupport = hollowThickColorCoresV2(xdogSupport);
        xdogSupport = cleanBinaryMaskForContours(xdogSupport);
    }

    if (options.saveIntermediates) {
        vector<IntermediateImageV2> intermediates;
        addIntermediateImageV2(intermediates, "input_resized", normalizeImageForDisplayV2(resized, resized.size()));
        addIntermediateImageV2(intermediates, "working_enhanced", normalizeImageForDisplayV2(workSrc, resized.size()));
        addIntermediateImageV2(intermediates, "subject_mask", normalizeMaskForDisplayV2(characterGuides.subjectMask, resized.size()));
        addIntermediateImageV2(intermediates, "character_guide", normalizeMaskForDisplayV2(characterGuides.characterGuide, resized.size()));
        addIntermediateImageV2(intermediates, "xdog_candidate", normalizeMaskForDisplayV2(xdogMask, resized.size()));
        addIntermediateImageV2(intermediates, "edge_support", normalizeMaskForDisplayV2(strongSupport, resized.size()));
        addIntermediateImageV2(intermediates, "fine_detail", normalizeMaskForDisplayV2(fineDetail, resized.size()));
        addIntermediateImageV2(intermediates, "filled_regions", normalizeMaskForDisplayV2(filledRegions, resized.size()));
        addIntermediateImageV2(intermediates, "region_outlines", normalizeMaskForDisplayV2(regionOutlines, resized.size()));
        addIntermediateImageV2(intermediates, "structural_support", normalizeMaskForDisplayV2(structuralSupport, resized.size()));
        addIntermediateImageV2(intermediates, "line_mask_clean", normalizeMaskForDisplayV2(lineMask, resized.size()));
        addIntermediateImageV2(intermediates, "support_mask", normalizeMaskForDisplayV2(characterGuides.characterSupport, resized.size()));
        addIntermediateImageV2(intermediates, "lower_body_soft_support", normalizeMaskForDisplayV2(lowerBodySoftSupport, resized.size()));
        addIntermediateImageV2(intermediates, "xdog_guide_final", normalizeMaskForDisplayV2(xdogGuide, resized.size()));
        addIntermediateImageV2(intermediates, "xdog_support_final", normalizeMaskForDisplayV2(xdogSupport, resized.size()));
        addIntermediateImageV2(intermediates, "guide_overlay",
                               makeOverlayPreviewV2(resized, xdogGuide, resized.size(), Scalar(44, 130, 240)));
        addIntermediateImageV2(intermediates, "support_overlay",
                               makeOverlayPreviewV2(resized, xdogSupport, resized.size(), Scalar(45, 185, 105)));
        writeIntermediateImagesV2(outputDir, intermediates);
        cout << "  Intermediate case images: " << outputDir << "\\intermediate\\\n";
        cout << "  Intermediate contact sheet: " << outputDir << "\\art_intermediate_case.png\n";
    } else {
        deleteIntermediateImagesV2(outputDir);
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
        } else if (arg == "--intermediate" || arg == "--save-intermediate") {
            options.saveIntermediates = true;
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
    cout << "Intermediate output: " << (options.saveIntermediates ? "enabled" : "disabled") << "\n";

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
