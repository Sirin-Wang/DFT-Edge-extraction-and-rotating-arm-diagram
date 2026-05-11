#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "v2_dft_precompute.h"

using namespace std;

static string dftPathStem(const string& path) {
    size_t slash = path.find_last_of("\\/");
    size_t start = slash == string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == string::npos || dot < start) dot = path.size();
    return path.substr(start, dot - start);
}

static vector<string> splitCsvDftMain(const string& text) {
    vector<string> values;
    string current;
    for (char ch : text) {
        if (ch == ',') {
            if (!current.empty()) values.push_back(current);
            current.clear();
        } else if (!isspace(static_cast<unsigned char>(ch))) {
            current.push_back(ch);
        }
    }
    if (!current.empty()) values.push_back(current);
    return values;
}

static vector<string> modesFromTextDftMain(const string& text) {
    if (text == "both") return {"sequential", "simultaneous"};
    if (text == "all") return {"sequential", "simultaneous", "full_svg"};
    vector<string> modes = splitCsvDftMain(text);
    vector<string> filtered;
    for (const string& mode : modes) {
        if (mode == "sequential" || mode == "simultaneous" || mode == "full_svg") filtered.push_back(mode);
    }
    return filtered.empty() ? vector<string>{"sequential", "simultaneous"} : filtered;
}

static vector<string> findResultImagesDftMain(const string& outputRoot) {
    vector<string> images;
    string pattern = outputRoot + "\\*";
    _finddata_t fileInfo{};
    intptr_t handle = _findfirst(pattern.c_str(), &fileInfo);
    if (handle == -1) return images;

    while (true) {
        string name = fileInfo.name;
        if (name != "." && name != ".." && (fileInfo.attrib & _A_SUBDIR)) {
            string compDir = outputRoot + "\\" + name + "\\comp";
            if (_access(compDir.c_str(), 0) == 0) images.push_back(name);
        }
        if (_findnext(handle, &fileInfo) != 0) break;
    }

    _findclose(handle);
    sort(images.begin(), images.end());
    return images;
}

static void printUsageDftMain() {
    cout << "Usage: DftPrecompute.exe [options] [art art1 art2]\n"
         << "Options:\n"
         << "  --output <dir>       Results root, default results_v2\n"
         << "  --config <file>      TXT config, default dft_scene_params.txt\n"
         << "  --channels <csv>     Default XDoG_Guide,XDoG_Support\n"
         << "  --mode <value>       sequential, simultaneous, full_svg, both, or all\n"
         << "  --modes <csv>        Same as --mode, accepts comma-separated values\n";
}

int main(int argc, char** argv) {
    cout << "===========================================\n";
    cout << "  DFT Scene Precompute\n";
    cout << "===========================================\n\n";

    string outputRoot = "results_v2";
    string configPath = "dft_scene_params.txt";
    vector<string> channels = {"XDoG_Guide", "XDoG_Support"};
    vector<string> imageNames;
    string modeOverride;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if ((arg == "--help" || arg == "-h")) {
            printUsageDftMain();
            return 0;
        } else if (arg == "--output" && i + 1 < argc) {
            outputRoot = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--channels" && i + 1 < argc) {
            channels = splitCsvDftMain(argv[++i]);
        } else if ((arg == "--mode" || arg == "--modes") && i + 1 < argc) {
            modeOverride = argv[++i];
        } else {
            imageNames.push_back(dftPathStem(arg));
        }
    }

    map<string, string> config = readDftConfigV2Dft(configPath);
    string action = configStringV2Dft(config, "action", "precompute");
    if (action != "precompute" && action != "both") {
        cout << "Config action=" << action << "; precompute skipped.\n";
        return 0;
    }

    if (imageNames.empty()) imageNames = findResultImagesDftMain(outputRoot);
    if (imageNames.empty()) {
        cout << "No result folders with comp found under " << outputRoot << "\n";
        return 1;
    }

    string modeText = modeOverride.empty() ? configStringV2Dft(config, "mode", "both") : modeOverride;
    vector<string> modes = modesFromTextDftMain(modeText);

    cout << "Output root: " << outputRoot << "\n";
    cout << "Config: " << configPath << "\n";
    cout << "Channels: ";
    for (size_t i = 0; i < channels.size(); ++i) cout << (i ? "," : "") << channels[i];
    cout << "\nModes: ";
    for (size_t i = 0; i < modes.size(); ++i) cout << (i ? "," : "") << modes[i];
    cout << "\n";

    int successCount = 0;
    for (const string& imageName : imageNames) {
        if (precomputeDftForImageV2(outputRoot, imageName, channels, modes, config)) {
            ++successCount;
        }
    }

    cout << "\nDFT precomputed " << successCount << " of " << imageNames.size() << " images\n";
    return successCount == static_cast<int>(imageNames.size()) ? 0 : 1;
}
