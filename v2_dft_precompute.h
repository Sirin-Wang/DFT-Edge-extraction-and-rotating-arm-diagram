#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <direct.h>
#include <io.h>

using namespace std;

struct V2DftPoint {
    double x = 0.0;
    double y = 0.0;
};

struct V2DftMatrix {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
};

struct V2DftLoop {
    string file;
    int loopIndex = 0;
    double length = 0.0;
    double start = 0.0;
    double end = 1.0;
    bool showArms = false;
    vector<V2DftPoint> points;
    vector<int> breaks;
    struct Coef {
        int freq = 0;
        double re = 0.0;
        double im = 0.0;
        double amp = 0.0;
    };
    vector<Coef> coeffs;
    vector<V2DftPoint> trace;
};

struct V2DftSceneParams {
    int samples = 512;
    int arms = 96;
    int minLoopSamples = 16;
    double minLoopLength = 0.0;
    double maxLoopLength = 0.0;
    int smoothPasses = 0;
    double duration = 60.0;
    double hold = 3.0;
    int simArmParts = 24;
    int targetFps = 30;
    int drawStride = 2;
    double componentTimePower = 1.0;
    string fullSvgOrder = "nearest";
    string coefficientOrder = "amplitude";
};

static void ensureDirectoryV2Dft(const string& dirName) {
    _mkdir(dirName.c_str());
}

static string readTextFileV2Dft(const string& path) {
    ifstream in(path, ios::in | ios::binary);
    if (!in) return "";
    stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

static string jsonEscapeV2Dft(const string& text) {
    string out;
    for (char ch : text) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else out += ch;
    }
    return out;
}

static vector<double> parseDoublesV2Dft(const string& text) {
    vector<double> values;
    static const regex numberRe(R"([-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)");
    for (sregex_iterator it(text.begin(), text.end(), numberRe), end; it != end; ++it) {
        values.push_back(atof(it->str().c_str()));
    }
    return values;
}

static V2DftMatrix multiplyMatrixV2Dft(const V2DftMatrix& a, const V2DftMatrix& b) {
    V2DftMatrix m;
    m.a = a.a * b.a + a.c * b.b;
    m.b = a.b * b.a + a.d * b.b;
    m.c = a.a * b.c + a.c * b.d;
    m.d = a.b * b.c + a.d * b.d;
    m.e = a.a * b.e + a.c * b.f + a.e;
    m.f = a.b * b.e + a.d * b.f + a.f;
    return m;
}

static V2DftPoint transformPointV2Dft(const V2DftPoint& p, const V2DftMatrix& m) {
    return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}

static V2DftMatrix parseTransformV2Dft(const string& svgText) {
    smatch match;
    regex groupRe(R"REGEX(<g[^>]*transform="([^"]+)")REGEX", regex::icase);
    if (!regex_search(svgText, match, groupRe)) return {};

    V2DftMatrix matrix;
    string transform = match[1].str();
    regex callRe(R"((translate|scale)\(([^)]*)\))", regex::icase);
    for (sregex_iterator it(transform.begin(), transform.end(), callRe), end; it != end; ++it) {
        string name = (*it)[1].str();
        vector<double> values = parseDoublesV2Dft((*it)[2].str());
        V2DftMatrix next;
        if (name == "translate" || name == "TRANSLATE") {
            next.e = values.empty() ? 0.0 : values[0];
            next.f = values.size() > 1 ? values[1] : 0.0;
        } else if (name == "scale" || name == "SCALE") {
            next.a = values.empty() ? 1.0 : values[0];
            next.d = values.size() > 1 ? values[1] : next.a;
        }
        matrix = multiplyMatrixV2Dft(matrix, next);
    }
    return matrix;
}

static vector<string> extractPathDataV2Dft(const string& svgText) {
    vector<string> paths;
    regex pathRe(R"REGEX(<path[^>]*\sd="([^"]*)")REGEX", regex::icase);
    for (sregex_iterator it(svgText.begin(), svgText.end(), pathRe), end; it != end; ++it) {
        paths.push_back((*it)[1].str());
    }
    return paths;
}

static vector<string> tokenizePathV2Dft(const string& d) {
    vector<string> tokens;
    regex tokenRe(R"([AaCcHhLlMmQqSsTtVvZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?)");
    for (sregex_iterator it(d.begin(), d.end(), tokenRe), end; it != end; ++it) {
        tokens.push_back(it->str());
    }
    return tokens;
}

static bool isCommandTokenV2Dft(const string& token) {
    return token.size() == 1 && isalpha(static_cast<unsigned char>(token[0]));
}

static double distanceV2Dft(const V2DftPoint& a, const V2DftPoint& b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

static V2DftPoint cubicPointV2Dft(const V2DftPoint& p0, const V2DftPoint& p1,
                                  const V2DftPoint& p2, const V2DftPoint& p3, double t) {
    double u = 1.0 - t;
    double uu = u * u;
    double tt = t * t;
    double uuu = uu * u;
    double ttt = tt * t;
    return {
        uuu * p0.x + 3.0 * uu * t * p1.x + 3.0 * u * tt * p2.x + ttt * p3.x,
        uuu * p0.y + 3.0 * uu * t * p1.y + 3.0 * u * tt * p2.y + ttt * p3.y
    };
}

static V2DftPoint quadPointV2Dft(const V2DftPoint& p0, const V2DftPoint& p1,
                                 const V2DftPoint& p2, double t) {
    double u = 1.0 - t;
    return {
        u * u * p0.x + 2.0 * u * t * p1.x + t * t * p2.x,
        u * u * p0.y + 2.0 * u * t * p1.y + t * t * p2.y
    };
}

static double polylineLengthV2Dft(const vector<V2DftPoint>& points) {
    double length = 0.0;
    for (size_t i = 1; i < points.size(); ++i) {
        length += distanceV2Dft(points[i - 1], points[i]);
    }
    return length;
}

static vector<V2DftPoint> resamplePolylineV2Dft(const vector<V2DftPoint>& points, int sampleCount) {
    vector<V2DftPoint> sampled;
    if (points.size() < 2 || sampleCount < 2) return sampled;

    double totalLength = polylineLengthV2Dft(points);
    if (totalLength <= 0.0) return sampled;

    sampled.reserve(sampleCount);
    size_t segment = 1;
    double traversed = 0.0;
    for (int i = 0; i < sampleCount; ++i) {
        double target = totalLength * static_cast<double>(i) / static_cast<double>(sampleCount);
        while (segment + 1 < points.size() &&
               traversed + distanceV2Dft(points[segment - 1], points[segment]) < target) {
            traversed += distanceV2Dft(points[segment - 1], points[segment]);
            ++segment;
        }

        double segmentLength = distanceV2Dft(points[segment - 1], points[segment]);
        double ratio = segmentLength > 0.0 ? (target - traversed) / segmentLength : 0.0;
        ratio = std::max(0.0, std::min(1.0, ratio));
        sampled.push_back({
            points[segment - 1].x + (points[segment].x - points[segment - 1].x) * ratio,
            points[segment - 1].y + (points[segment].y - points[segment - 1].y) * ratio
        });
    }
    return sampled;
}

static bool isClosedPolylineV2Dft(const vector<V2DftPoint>& points) {
    if (points.size() < 3) return false;
    double length = polylineLengthV2Dft(points);
    double tolerance = std::max(0.001, length * 0.000001);
    return distanceV2Dft(points.front(), points.back()) <= tolerance;
}

static vector<V2DftPoint> smoothPolylineV2Dft(const vector<V2DftPoint>& points, int passes, bool closed) {
    if (points.size() < 3 || passes <= 0) return points;
    vector<V2DftPoint> current = points;
    vector<V2DftPoint> next = points;
    size_t workCount = current.size();
    if (closed && current.size() > 3) {
        double tolerance = std::max(0.001, polylineLengthV2Dft(current) * 0.000001);
        if (distanceV2Dft(current.front(), current.back()) <= tolerance) {
            workCount = current.size() - 1;
        }
    }

    for (int pass = 0; pass < passes; ++pass) {
        for (size_t i = 0; i < workCount; ++i) {
            if (!closed && (i == 0 || i + 1 == current.size())) {
                next[i] = current[i];
                continue;
            }
            size_t prev = i == 0 ? workCount - 1 : i - 1;
            size_t nextIndex = i + 1 == workCount ? 0 : i + 1;
            next[i] = {
                current[i].x * 0.50 + (current[prev].x + current[nextIndex].x) * 0.25,
                current[i].y * 0.50 + (current[prev].y + current[nextIndex].y) * 0.25
            };
        }
        if (closed && workCount < next.size()) next[workCount] = next[0];
        current.swap(next);
    }
    return current;
}

static vector<vector<V2DftPoint>> splitPolylineByLengthV2Dft(const vector<V2DftPoint>& points, double maxLength) {
    vector<vector<V2DftPoint>> parts;
    if (points.size() < 2 || maxLength <= 0.0) {
        if (points.size() > 1) parts.push_back(points);
        return parts;
    }

    vector<V2DftPoint> current;
    current.push_back(points.front());
    double currentLength = 0.0;

    for (size_t i = 1; i < points.size(); ++i) {
        V2DftPoint from = points[i - 1];
        V2DftPoint to = points[i];
        double segmentLength = distanceV2Dft(from, to);
        if (segmentLength <= 0.0) continue;

        double consumed = 0.0;
        while (consumed < segmentLength) {
            double remainingSegment = segmentLength - consumed;
            double remainingPart = maxLength - currentLength;
            if (remainingPart <= 0.0) {
                if (current.size() > 1) parts.push_back(current);
                current.clear();
                current.push_back(from);
                currentLength = 0.0;
                remainingPart = maxLength;
            }

            if (remainingSegment <= remainingPart) {
                current.push_back(to);
                currentLength += remainingSegment;
                consumed = segmentLength;
            } else {
                double t = (consumed + remainingPart) / segmentLength;
                V2DftPoint splitPoint{
                    from.x + (to.x - from.x) * t,
                    from.y + (to.y - from.y) * t
                };
                current.push_back(splitPoint);
                if (current.size() > 1) parts.push_back(current);
                current.clear();
                current.push_back(splitPoint);
                currentLength = 0.0;
                consumed += remainingPart;
            }
        }
    }

    if (current.size() > 1) parts.push_back(current);
    return parts;
}

static vector<int> allocateSampleCountsV2Dft(const vector<double>& lengths, int targetCount, int preferredMinCount = 1) {
    vector<int> counts(lengths.size(), 0);
    if (lengths.empty() || targetCount <= 0) return counts;

    double totalLength = 0.0;
    for (double length : lengths) totalLength += std::max(0.0, length);
    if (totalLength <= 0.0) return counts;

    preferredMinCount = std::max(0, preferredMinCount);
    int minCount = preferredMinCount;
    if (static_cast<long long>(minCount) * static_cast<long long>(lengths.size()) > targetCount) {
        minCount = static_cast<int>(lengths.size()) <= targetCount ? 1 : 0;
    }
    vector<double> remainders(lengths.size(), 0.0);
    int countSum = 0;
    for (size_t i = 0; i < lengths.size(); ++i) {
        double exact = targetCount * std::max(0.0, lengths[i]) / totalLength;
        int base = std::max(minCount, static_cast<int>(floor(exact)));
        counts[i] = base;
        remainders[i] = exact - floor(exact);
        countSum += base;
    }

    while (countSum > targetCount) {
        int best = -1;
        for (size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] <= minCount) continue;
            if (best < 0 || remainders[i] < remainders[best]) best = static_cast<int>(i);
        }
        if (best < 0) break;
        --counts[best];
        --countSum;
    }

    while (countSum < targetCount) {
        int best = 0;
        for (size_t i = 1; i < remainders.size(); ++i) {
            if (remainders[i] > remainders[best]) best = static_cast<int>(i);
        }
        ++counts[best];
        remainders[best] = 0.0;
        ++countSum;
    }

    return counts;
}

static vector<vector<V2DftPoint>> flattenPathV2Dft(const string& d, const V2DftMatrix& transform) {
    vector<vector<V2DftPoint>> loops;
    vector<V2DftPoint> loop;
    vector<string> tokens = tokenizePathV2Dft(d);
    size_t index = 0;
    char command = 0;
    V2DftPoint current{0.0, 0.0};
    V2DftPoint subpathStart{0.0, 0.0};
    V2DftPoint lastCubicControl{0.0, 0.0};
    V2DftPoint lastQuadControl{0.0, 0.0};
    char previousCommand = 0;

    auto numberAt = [&]() -> double { return atof(tokens[index++].c_str()); };
    auto finishLoop = [&]() {
        if (loop.size() > 1) {
            loops.push_back(loop);
        }
        loop.clear();
    };
    auto addRawPoint = [&](const V2DftPoint& p) {
        loop.push_back(transformPointV2Dft(p, transform));
    };
    auto readPoint = [&](bool relative) -> V2DftPoint {
        double x = numberAt();
        double y = numberAt();
        if (relative) return {current.x + x, current.y + y};
        return {x, y};
    };
    auto hasNumbers = [&]() -> bool {
        return index < tokens.size() && !isCommandTokenV2Dft(tokens[index]);
    };

    while (index < tokens.size()) {
        if (isCommandTokenV2Dft(tokens[index])) {
            command = tokens[index++][0];
        } else if (command == 0) {
            break;
        }

        char upper = static_cast<char>(toupper(static_cast<unsigned char>(command)));
        bool relative = command != upper;

        if (upper == 'Z') {
            addRawPoint(subpathStart);
            current = subpathStart;
            finishLoop();
            previousCommand = command;
            command = 0;
            continue;
        }

        if (upper == 'M') {
            finishLoop();
            if (index + 1 > tokens.size()) break;
            current = readPoint(relative);
            subpathStart = current;
            addRawPoint(current);
            while (hasNumbers() && index + 1 < tokens.size()) {
                current = readPoint(relative);
                addRawPoint(current);
            }
            command = relative ? 'l' : 'L';
            previousCommand = 'M';
            continue;
        }

        while (hasNumbers()) {
            if (upper == 'L') {
                current = readPoint(relative);
                addRawPoint(current);
            } else if (upper == 'H') {
                double x = numberAt();
                current.x = relative ? current.x + x : x;
                addRawPoint(current);
            } else if (upper == 'V') {
                double y = numberAt();
                current.y = relative ? current.y + y : y;
                addRawPoint(current);
            } else if (upper == 'C') {
                V2DftPoint p0 = current;
                V2DftPoint p1 = readPoint(relative);
                V2DftPoint p2 = readPoint(relative);
                V2DftPoint p3 = readPoint(relative);
                for (int i = 1; i <= 36; ++i) {
                    addRawPoint(cubicPointV2Dft(p0, p1, p2, p3, i / 36.0));
                }
                current = p3;
                lastCubicControl = p2;
            } else if (upper == 'S') {
                V2DftPoint p0 = current;
                V2DftPoint p1 = (toupper(static_cast<unsigned char>(previousCommand)) == 'C' ||
                                 toupper(static_cast<unsigned char>(previousCommand)) == 'S')
                                    ? V2DftPoint{2.0 * current.x - lastCubicControl.x,
                                                 2.0 * current.y - lastCubicControl.y}
                                    : current;
                V2DftPoint p2 = readPoint(relative);
                V2DftPoint p3 = readPoint(relative);
                for (int i = 1; i <= 36; ++i) {
                    addRawPoint(cubicPointV2Dft(p0, p1, p2, p3, i / 36.0));
                }
                current = p3;
                lastCubicControl = p2;
            } else if (upper == 'Q') {
                V2DftPoint p0 = current;
                V2DftPoint p1 = readPoint(relative);
                V2DftPoint p2 = readPoint(relative);
                for (int i = 1; i <= 24; ++i) {
                    addRawPoint(quadPointV2Dft(p0, p1, p2, i / 24.0));
                }
                current = p2;
                lastQuadControl = p1;
            } else if (upper == 'T') {
                V2DftPoint p0 = current;
                V2DftPoint p1 = (toupper(static_cast<unsigned char>(previousCommand)) == 'Q' ||
                                 toupper(static_cast<unsigned char>(previousCommand)) == 'T')
                                    ? V2DftPoint{2.0 * current.x - lastQuadControl.x,
                                                 2.0 * current.y - lastQuadControl.y}
                                    : current;
                V2DftPoint p2 = readPoint(relative);
                for (int i = 1; i <= 24; ++i) {
                    addRawPoint(quadPointV2Dft(p0, p1, p2, i / 24.0));
                }
                current = p2;
                lastQuadControl = p1;
            } else if (upper == 'A') {
                if (index + 6 >= tokens.size()) break;
                index += 5;
                current = readPoint(relative);
                addRawPoint(current);
            } else {
                break;
            }
            previousCommand = command;
        }
    }
    finishLoop();
    return loops;
}

static V2DftLoop::Coef computeDftCoefficientV2Dft(const vector<V2DftPoint>& points, int freq) {
    int n = static_cast<int>(points.size());
    const double tau = 6.28318530717958647692;
    complex<double> sum(0.0, 0.0);
    double angleStep = -tau * static_cast<double>(freq) / static_cast<double>(n);
    complex<double> step(cos(angleStep), sin(angleStep));
    complex<double> rot(1.0, 0.0);
    for (int i = 0; i < n; ++i) {
        sum += complex<double>(points[i].x, points[i].y) * rot;
        rot *= step;
    }
    sum /= static_cast<double>(n);
    return {freq, sum.real(), sum.imag(), abs(sum)};
}

static vector<V2DftLoop::Coef> computeDftCoefficientsV2Dft(const vector<V2DftPoint>& points, int arms,
                                                           const string& orderMode = "amplitude") {
    vector<V2DftLoop::Coef> coeffs;
    int n = static_cast<int>(points.size());
    if (n <= 1) return coeffs;

    if (orderMode == "frequency") {
        vector<V2DftLoop::Coef> ordered;
        ordered.reserve(std::min(n, arms + 1));
        ordered.push_back(computeDftCoefficientV2Dft(points, 0));
        for (int harmonic = 1; static_cast<int>(ordered.size()) < arms + 1 && harmonic <= n / 2; ++harmonic) {
            ordered.push_back(computeDftCoefficientV2Dft(points, harmonic));
            if (static_cast<int>(ordered.size()) >= arms + 1) break;
            if (!(n % 2 == 0 && harmonic == n / 2)) {
                ordered.push_back(computeDftCoefficientV2Dft(points, -harmonic));
            }
        }
        return ordered;
    }

    coeffs.reserve(n);
    for (int k = 0; k < n; ++k) {
        int freq = k <= n / 2 ? k : k - n;
        coeffs.push_back(computeDftCoefficientV2Dft(points, freq));
    }

    auto dc = find_if(coeffs.begin(), coeffs.end(), [](const V2DftLoop::Coef& c) { return c.freq == 0; });
    vector<V2DftLoop::Coef> ordered;
    if (dc != coeffs.end()) ordered.push_back(*dc);
    vector<V2DftLoop::Coef> rest;
    for (const auto& coef : coeffs) {
        if (coef.freq != 0) rest.push_back(coef);
    }

    sort(rest.begin(), rest.end(), [](const auto& a, const auto& b) { return a.amp > b.amp; });
    for (const auto& coef : rest) {
        if (static_cast<int>(ordered.size()) >= arms + 1) break;
        ordered.push_back(coef);
    }
    return ordered;
}

static V2DftPoint reconstructPointV2Dft(double t, const vector<V2DftLoop::Coef>& coeffs) {
    const double tau = 6.28318530717958647692;
    V2DftPoint p;
    for (const auto& coef : coeffs) {
        double angle = tau * coef.freq * t;
        double c = cos(angle);
        double s = sin(angle);
        p.x += coef.re * c - coef.im * s;
        p.y += coef.re * s + coef.im * c;
    }
    return p;
}

static vector<V2DftPoint> buildTraceV2Dft(int count, const vector<V2DftLoop::Coef>& coeffs) {
    vector<V2DftPoint> trace;
    trace.reserve(std::max(0, count));
    for (int i = 0; i < count; ++i) {
        trace.push_back(reconstructPointV2Dft(static_cast<double>(i) / static_cast<double>(count), coeffs));
    }
    return trace;
}

static map<string, string> readDftConfigV2Dft(const string& configPath) {
    map<string, string> config;
    ifstream in(configPath);
    string line;
    while (getline(in, line)) {
        size_t comment = line.find('#');
        if (comment != string::npos) line = line.substr(0, comment);
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string key = line.substr(0, eq);
        string value = line.substr(eq + 1);
        auto trim = [](string s) {
            while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        };
        key = trim(key);
        value = trim(value);
        if (!key.empty()) config[key] = value;
    }
    return config;
}

static string configStringV2Dft(const map<string, string>& config, const string& key, const string& fallback) {
    auto it = config.find(key);
    return it == config.end() ? fallback : it->second;
}

static string modeConfigStringV2Dft(const map<string, string>& config, const string& mode,
                                    const string& key, const string& fallback) {
    return configStringV2Dft(config, mode + "." + key, configStringV2Dft(config, key, fallback));
}

static string lowerStringV2Dft(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    return value;
}

static int configIntV2Dft(const map<string, string>& config, const string& mode,
                          const string& key, int fallback, int minValue, int maxValue) {
    int value = atoi(modeConfigStringV2Dft(config, mode, key, to_string(fallback)).c_str());
    return std::max(minValue, std::min(maxValue, value));
}

static double configDoubleV2Dft(const map<string, string>& config, const string& mode,
                                const string& key, double fallback, double minValue, double maxValue) {
    double value = atof(modeConfigStringV2Dft(config, mode, key, to_string(fallback)).c_str());
    return std::max(minValue, std::min(maxValue, value));
}

static V2DftSceneParams paramsForModeV2Dft(const map<string, string>& config, const string& mode) {
    V2DftSceneParams params;
    int defaultSamples = mode == "sequential" ? 1500 : (mode == "full_svg" ? 128 : 512);
    int maxSamples = mode == "full_svg" ? 65536 : 4096;
    params.samples = configIntV2Dft(config, mode, "samples", defaultSamples, 64, maxSamples);
    params.arms = configIntV2Dft(config, mode, "arms", mode == "sequential" ? 240 : (mode == "full_svg" ? 1024 : 96), 1, 262143);
    params.minLoopSamples = configIntV2Dft(config, mode, "min_loop_samples",
                                           mode == "sequential" ? 24 : (mode == "simultaneous" ? 16 : 2),
                                           2, 128);
    params.minLoopLength = configDoubleV2Dft(config, mode, "min_loop_length",
                                             mode == "full_svg" ? 6.0 : 12.0, 0.0, 1000.0);
    params.maxLoopLength = configDoubleV2Dft(config, mode, "max_loop_length",
                                             mode == "full_svg" ? 0.0 : 260.0, 0.0, 1000000.0);
    params.smoothPasses = configIntV2Dft(config, mode, "smooth_passes",
                                         mode == "full_svg" ? 1 : 1, 0, 8);
    params.duration = configDoubleV2Dft(config, mode, "duration", 60.0, 3.0, 600.0);
    params.hold = configDoubleV2Dft(config, mode, "hold", 3.0, 0.0, 120.0);
    params.simArmParts = configIntV2Dft(config, mode, "sim_arm_parts", 24, 0, 9999);
    params.targetFps = configIntV2Dft(config, mode, "target_fps", mode == "simultaneous" ? 24 : 30, 0, 120);
    params.drawStride = configIntV2Dft(config, mode, "draw_stride", mode == "simultaneous" ? 3 : (mode == "full_svg" ? 2 : 1), 1, 16);
    params.componentTimePower = configDoubleV2Dft(config, mode, "component_time_power",
                                                  mode == "sequential" ? 0.70 : 1.0, 0.0, 1.5);
    params.fullSvgOrder = lowerStringV2Dft(modeConfigStringV2Dft(config, mode, "order",
                                                                 mode == "full_svg" ? "nearest" : "component"));
    if (params.fullSvgOrder != "component" && params.fullSvgOrder != "nearest") {
        params.fullSvgOrder = mode == "full_svg" ? "nearest" : "component";
    }
    params.coefficientOrder = lowerStringV2Dft(modeConfigStringV2Dft(config, mode, "coefficient_order",
                                                                     mode == "full_svg" ? "frequency" : "amplitude"));
    if (params.coefficientOrder != "amplitude" && params.coefficientOrder != "frequency") {
        params.coefficientOrder = mode == "full_svg" ? "frequency" : "amplitude";
    }
    if (mode != "full_svg") {
        params.arms = std::min(params.arms, std::max(1, params.samples - 1));
    }
    return params;
}

static void writePointArrayV2Dft(ofstream& out, const vector<V2DftPoint>& points) {
    out << "[";
    for (size_t i = 0; i < points.size(); ++i) {
        if (i) out << ",";
        out << "[" << fixed << setprecision(2) << points[i].x << "," << points[i].y << "]";
    }
    out << "]";
}

static void writeCoeffArrayV2Dft(ofstream& out, const vector<V2DftLoop::Coef>& coeffs) {
    out << "[";
    for (size_t i = 0; i < coeffs.size(); ++i) {
        if (i) out << ",";
        out << "[" << coeffs[i].freq << "," << fixed << setprecision(6)
            << coeffs[i].re << "," << coeffs[i].im << "," << coeffs[i].amp << "]";
    }
    out << "]";
}

static string removeExtensionV2Dft(const string& fileName) {
    size_t dot = fileName.find_last_of('.');
    return dot == string::npos ? fileName : fileName.substr(0, dot);
}

static bool writeFourierCoefficientFilesV2Dft(const string& resultDir, const string& image,
                                              const string& channel, const string& mode,
                                              const V2DftSceneParams& params,
                                              const vector<V2DftLoop>& loops,
                                              const map<string, vector<size_t>>& groupsByFile) {
    string coeffDir = resultDir + "\\FourierCoefficient";
    ensureDirectoryV2Dft(coeffDir);

    bool ok = true;
    for (const auto& group : groupsByFile) {
        string outputPath = coeffDir + "\\" + removeExtensionV2Dft(group.first) + ".txt";
        ofstream out(outputPath, ios::out | ios::binary);
        if (!out) {
            cout << "  Failed to write coefficient file: " << outputPath << "\n";
            ok = false;
            continue;
        }

        out << "# Fourier coefficients generated by DftPrecompute.exe\n";
        out << "image=" << image << "\n";
        out << "channel=" << channel << "\n";
        out << "component=" << group.first << "\n";
        out << "source_mode=" << mode << "\n";
        out << "samples=" << params.samples << "\n";
        out << "arms=" << params.arms << "\n";
        out << "loop_count=" << group.second.size() << "\n";
        out << "# columns: loop_index\tcoef_order\tfreq\tre\tim\tamp\tphase\n";
        out << fixed << setprecision(10);

        for (size_t loopOrder = 0; loopOrder < group.second.size(); ++loopOrder) {
            const V2DftLoop& loop = loops[group.second[loopOrder]];
            out << "\n";
            out << "# loop_index=" << loop.loopIndex
                << ", length=" << setprecision(6) << loop.length
                << ", sample_count=" << loop.points.size() << "\n";
            out << setprecision(10);
            for (size_t coefOrder = 0; coefOrder < loop.coeffs.size(); ++coefOrder) {
                const auto& coef = loop.coeffs[coefOrder];
                out << loop.loopIndex << "\t"
                    << coefOrder << "\t"
                    << coef.freq << "\t"
                    << coef.re << "\t"
                    << coef.im << "\t"
                    << coef.amp << "\t"
                    << atan2(coef.im, coef.re) << "\n";
            }
        }
    }

    return ok;
}

static bool writeDftSceneJsonV2Dft(const string& outputPath, const string& image, const string& channel,
                                   const string& mode, const V2DftSceneParams& params,
                                   const vector<V2DftLoop>& loops, double width, double height) {
    ofstream out(outputPath, ios::out | ios::binary);
    if (!out) return false;

    out << "{\n";
    out << "\"image\":\"" << jsonEscapeV2Dft(image) << "\",";
    out << "\"channel\":\"" << jsonEscapeV2Dft(channel) << "\",";
    out << "\"mode\":\"" << jsonEscapeV2Dft(mode) << "\",";
    out << "\"viewBox\":{\"x\":0,\"y\":0,\"width\":" << fixed << setprecision(2) << width
        << ",\"height\":" << height << "},";
    out << "\"samples\":" << params.samples << ",\"arms\":" << params.arms
        << ",\"minLoopSamples\":" << params.minLoopSamples
        << ",\"minLoopLength\":" << fixed << setprecision(3) << params.minLoopLength
        << ",\"maxLoopLength\":" << fixed << setprecision(3) << params.maxLoopLength
        << ",\"smoothPasses\":" << params.smoothPasses
        << ",\"duration\":" << params.duration << ",\"hold\":" << params.hold
        << ",\"simArmParts\":" << params.simArmParts << ",\"targetFps\":" << params.targetFps
        << ",\"drawStride\":" << params.drawStride
        << ",\"componentTimePower\":" << fixed << setprecision(3) << params.componentTimePower << ","
        << "\"fullSvgOrder\":\"" << jsonEscapeV2Dft(params.fullSvgOrder) << "\","
        << "\"coefficientOrder\":\"" << jsonEscapeV2Dft(params.coefficientOrder) << "\",";
    out << "\"components\":[\n";
    for (size_t i = 0; i < loops.size(); ++i) {
        const auto& loop = loops[i];
        if (i) out << ",\n";
        out << "{";
        out << "\"file\":\"" << jsonEscapeV2Dft(loop.file) << "\",";
        out << "\"loopIndex\":" << loop.loopIndex << ",";
        out << "\"length\":" << fixed << setprecision(3) << loop.length << ",";
        out << "\"start\":" << fixed << setprecision(8) << loop.start << ",";
        out << "\"end\":" << fixed << setprecision(8) << loop.end << ",";
        out << "\"showArms\":" << (loop.showArms ? "true" : "false") << ",";
        out << "\"points\":";
        writePointArrayV2Dft(out, loop.points);
        out << ",\"breaks\":[";
        for (size_t breakIndex = 0; breakIndex < loop.breaks.size(); ++breakIndex) {
            if (breakIndex) out << ",";
            out << loop.breaks[breakIndex];
        }
        out << "]";
        out << ",\"trace\":";
        writePointArrayV2Dft(out, loop.trace);
        out << ",\"coeffs\":";
        writeCoeffArrayV2Dft(out, loop.coeffs);
        out << "}";
    }
    out << "\n]}\n";
    return true;
}

static vector<string> listComponentSvgsV2Dft(const string& compDir, const string& channel) {
    vector<string> files;
    string pattern = compDir + "\\" + channel + "_*.svg";
    _finddata_t fileInfo{};
    intptr_t handle = _findfirst(pattern.c_str(), &fileInfo);
    if (handle == -1) return files;
    while (true) {
        if ((fileInfo.attrib & _A_SUBDIR) == 0) files.push_back(fileInfo.name);
        if (_findnext(handle, &fileInfo) != 0) break;
    }
    _findclose(handle);
    sort(files.begin(), files.end());
    return files;
}

static bool parseViewBoxSizeV2Dft(const string& svgText, double& width, double& height) {
    smatch match;
    regex viewBoxRe(R"REGEX(viewBox="([^"]+)")REGEX", regex::icase);
    if (!regex_search(svgText, match, viewBoxRe)) return false;
    vector<double> values = parseDoublesV2Dft(match[1].str());
    if (values.size() != 4) return false;
    width = values[2];
    height = values[3];
    return true;
}

struct V2DftFlattenedSource {
    string file;
    int loopIndex = 0;
    double length = 0.0;
    vector<V2DftPoint> points;
    bool reversed = false;
    size_t startOffset = 0;
};

static vector<V2DftFlattenedSource> collectFlattenedLoopsV2Dft(const string& compDir,
                                                               const vector<string>& files,
                                                               double& width,
                                                               double& height) {
    vector<V2DftFlattenedSource> flattenedSources;

    for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const string& file = files[fileIndex];
        cout << "    [" << (fileIndex + 1) << "/" << files.size() << "] " << file << "...\n";
        cout.flush();

        string svgPath = compDir + "\\" + file;
        string svgText = readTextFileV2Dft(svgPath);
        if (svgText.empty()) continue;
        if (width <= 0.0 || height <= 0.0) parseViewBoxSizeV2Dft(svgText, width, height);

        V2DftMatrix transform = parseTransformV2Dft(svgText);
        vector<string> pathData = extractPathDataV2Dft(svgText);
        int loopIndex = 0;
        for (const string& d : pathData) {
            vector<vector<V2DftPoint>> pathLoops = flattenPathV2Dft(d, transform);
            for (auto& loop : pathLoops) {
                double length = polylineLengthV2Dft(loop);
                if (length <= 0.0) continue;
                flattenedSources.push_back({file, loopIndex++, length, loop});
            }
        }

        cout << "      " << loopIndex << " loop(s)\n";
    }

    return flattenedSources;
}

static bool isClosedFlattenedLoopV2Dft(const V2DftFlattenedSource& loop) {
    if (loop.points.size() < 3) return false;
    double tolerance = std::max(0.001, loop.length * 0.000001);
    return distanceV2Dft(loop.points.front(), loop.points.back()) <= tolerance;
}

static size_t drawablePointCountV2Dft(const V2DftFlattenedSource& loop) {
    if (!isClosedFlattenedLoopV2Dft(loop)) return loop.points.size();
    return loop.points.size() > 0 ? loop.points.size() - 1 : 0;
}

static V2DftPoint loopTraversalStartV2Dft(const V2DftFlattenedSource& loop) {
    if (loop.points.empty()) return {};
    if (isClosedFlattenedLoopV2Dft(loop)) {
        size_t count = drawablePointCountV2Dft(loop);
        size_t offset = count > 0 ? std::min(loop.startOffset, count - 1) : 0;
        return loop.points[offset];
    }
    return loop.reversed ? loop.points.back() : loop.points.front();
}

static V2DftPoint loopTraversalEndV2Dft(const V2DftFlattenedSource& loop) {
    if (loop.points.empty()) return {};
    if (isClosedFlattenedLoopV2Dft(loop)) return loopTraversalStartV2Dft(loop);
    return loop.reversed ? loop.points.front() : loop.points.back();
}

static vector<V2DftPoint> traversalPointsV2Dft(const V2DftFlattenedSource& loop) {
    if (loop.points.empty()) return {};
    if (isClosedFlattenedLoopV2Dft(loop)) {
        size_t count = drawablePointCountV2Dft(loop);
        if (count < 2) return loop.points;

        size_t offset = std::min(loop.startOffset, count - 1);
        vector<V2DftPoint> ordered;
        ordered.reserve(count + 1);
        for (size_t i = 0; i < count; ++i) {
            size_t index = loop.reversed
                ? (offset + count - i) % count
                : (offset + i) % count;
            ordered.push_back(loop.points[index]);
        }
        ordered.push_back(ordered.front());
        return ordered;
    }

    if (!loop.reversed) return loop.points;
    vector<V2DftPoint> reversed(loop.points.rbegin(), loop.points.rend());
    return reversed;
}

struct V2DftLoopEntryChoice {
    double distance = numeric_limits<double>::max();
    bool reversed = false;
    size_t startOffset = 0;
};

static V2DftLoopEntryChoice nearestLoopEntryV2Dft(const V2DftPoint& current,
                                                  const V2DftFlattenedSource& loop) {
    V2DftLoopEntryChoice choice;
    if (loop.points.empty()) return choice;

    if (isClosedFlattenedLoopV2Dft(loop)) {
        size_t count = drawablePointCountV2Dft(loop);
        for (size_t i = 0; i < count; ++i) {
            double distance = distanceV2Dft(current, loop.points[i]);
            if (distance < choice.distance) {
                choice.distance = distance;
                choice.startOffset = i;
            }
        }
        return choice;
    }

    double startDistance = distanceV2Dft(current, loop.points.front());
    double endDistance = distanceV2Dft(current, loop.points.back());
    if (endDistance < startDistance) {
        choice.distance = endDistance;
        choice.reversed = true;
    } else {
        choice.distance = startDistance;
    }
    return choice;
}

struct V2DftFlattenedGroup {
    string file;
    vector<V2DftFlattenedSource> loops;
};

static vector<V2DftFlattenedGroup> groupFlattenedLoopsByFileV2Dft(const vector<V2DftFlattenedSource>& loops) {
    vector<V2DftFlattenedGroup> groups;
    for (const auto& loop : loops) {
        if (groups.empty() || groups.back().file != loop.file) {
            V2DftFlattenedGroup group;
            group.file = loop.file;
            groups.push_back(std::move(group));
        }
        groups.back().loops.push_back(loop);
    }
    return groups;
}

static V2DftLoopEntryChoice nearestGroupEntryV2Dft(const V2DftPoint& current,
                                                   const V2DftFlattenedGroup& group) {
    V2DftLoopEntryChoice best;
    for (const auto& loop : group.loops) {
        V2DftLoopEntryChoice choice = nearestLoopEntryV2Dft(current, loop);
        if (choice.distance < best.distance) best = choice;
    }
    return best;
}

static V2DftPoint groupCentroidV2Dft(const V2DftFlattenedGroup& group) {
    V2DftPoint centroid;
    double totalWeight = 0.0;
    for (const auto& loop : group.loops) {
        if (loop.points.empty()) continue;
        double weight = std::max(1.0, loop.length);
        V2DftPoint loopCenter;
        size_t count = drawablePointCountV2Dft(loop);
        count = count > 0 ? count : loop.points.size();
        for (size_t i = 0; i < count; ++i) {
            loopCenter.x += loop.points[i].x;
            loopCenter.y += loop.points[i].y;
        }
        loopCenter.x /= static_cast<double>(count);
        loopCenter.y /= static_cast<double>(count);
        centroid.x += loopCenter.x * weight;
        centroid.y += loopCenter.y * weight;
        totalWeight += weight;
    }
    if (totalWeight > 0.0) {
        centroid.x /= totalWeight;
        centroid.y /= totalWeight;
    }
    return centroid;
}

static size_t outermostGroupIndexV2Dft(const vector<V2DftFlattenedGroup>& groups, double width, double height) {
    if (groups.empty()) return 0;
    V2DftPoint center{width * 0.5, height * 0.5};
    size_t best = 0;
    double bestDistance = -1.0;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].loops.empty()) continue;
        V2DftPoint centroid = groupCentroidV2Dft(groups[i]);
        double distance = distanceV2Dft(centroid, center);
        if (distance > bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

static vector<V2DftFlattenedSource> orderLoopsNearestV2Dft(const vector<V2DftFlattenedSource>& loops,
                                                           bool hasCurrent,
                                                           V2DftPoint& current) {
    vector<V2DftFlattenedSource> ordered;
    vector<char> used(loops.size(), 0);
    ordered.reserve(loops.size());

    for (size_t step = 0; step < loops.size(); ++step) {
        size_t bestIndex = loops.size();
        V2DftLoopEntryChoice bestChoice;

        if (!hasCurrent && step == 0) {
            for (size_t i = 0; i < loops.size(); ++i) {
                if (!loops[i].points.empty()) {
                    bestIndex = i;
                    break;
                }
            }
        } else {
            for (size_t i = 0; i < loops.size(); ++i) {
                if (used[i] || loops[i].points.empty()) continue;
                V2DftLoopEntryChoice choice = nearestLoopEntryV2Dft(current, loops[i]);
                if (choice.distance < bestChoice.distance) {
                    bestChoice = choice;
                    bestIndex = i;
                }
            }
        }

        if (bestIndex >= loops.size()) break;
        used[bestIndex] = 1;
        V2DftFlattenedSource selected = loops[bestIndex];
        if (hasCurrent || step > 0) {
            selected.reversed = bestChoice.reversed;
            selected.startOffset = bestChoice.startOffset;
        }
        ordered.push_back(selected);
        current = loopTraversalEndV2Dft(selected);
        hasCurrent = true;
    }

    return ordered;
}

static vector<V2DftFlattenedSource> orderFlattenedLoopsV2Dft(const vector<V2DftFlattenedSource>& flattened,
                                                             const string& orderMode,
                                                             double width,
                                                             double height) {
    if (orderMode != "nearest" || flattened.size() < 2) return flattened;

    vector<V2DftFlattenedGroup> groups = groupFlattenedLoopsByFileV2Dft(flattened);
    if (groups.size() < 2) {
        V2DftPoint current;
        return orderLoopsNearestV2Dft(flattened, false, current);
    }

    vector<V2DftFlattenedSource> ordered;
    vector<char> used(groups.size(), 0);
    ordered.reserve(flattened.size());

    bool hasCurrent = false;
    V2DftPoint current;
    for (size_t step = 0; step < groups.size(); ++step) {
        size_t bestGroup = groups.size();
        if (!hasCurrent && step == 0) {
            bestGroup = outermostGroupIndexV2Dft(groups, width, height);
        } else {
            double bestDistance = numeric_limits<double>::max();
            for (size_t i = 0; i < groups.size(); ++i) {
                if (used[i] || groups[i].loops.empty()) continue;
                double distance = nearestGroupEntryV2Dft(current, groups[i]).distance;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestGroup = i;
                }
            }
        }

        if (bestGroup >= groups.size()) break;
        used[bestGroup] = 1;
        vector<V2DftFlattenedSource> componentLoops =
            orderLoopsNearestV2Dft(groups[bestGroup].loops, hasCurrent, current);
        ordered.insert(ordered.end(), componentLoops.begin(), componentLoops.end());
        hasCurrent = !ordered.empty();
    }

    return ordered.empty() ? flattened : ordered;
}

static vector<V2DftPoint> resampleFlattenedLoopV2Dft(const V2DftFlattenedSource& loop, int sampleCount) {
    vector<V2DftPoint> points = traversalPointsV2Dft(loop);
    return resamplePolylineV2Dft(points, sampleCount);
}

static bool precomputeFullSvgDftSceneV2(const string& resultDir, const string& image, const string& channel,
                                        const string& mode, const V2DftSceneParams& params,
                                        bool writeCoefficientFiles) {
    string compDir = resultDir + "\\comp";
    vector<string> files = listComponentSvgsV2Dft(compDir, channel);
    if (files.empty()) {
        cout << "  No component SVGs for " << image << " / " << channel << "\n";
        return false;
    }

    double width = 0.0, height = 0.0;
    vector<V2DftFlattenedSource> flattened = collectFlattenedLoopsV2Dft(compDir, files, width, height);
    if (flattened.empty()) {
        cout << "  No drawable loops for full_svg " << image << " / " << channel << "\n";
        return false;
    }
    flattened = orderFlattenedLoopsV2Dft(flattened, params.fullSvgOrder, width, height);

    vector<double> lengths;
    lengths.reserve(flattened.size());
    double totalLength = 0.0;
    for (const auto& loop : flattened) {
        if (loop.length < params.minLoopLength) continue;
        lengths.push_back(loop.length);
        totalLength += loop.length;
    }
    if (lengths.empty() || totalLength <= 0.0) {
        cout << "  No loops longer than min_loop_length for full_svg " << image << " / " << channel << "\n";
        return false;
    }

    int targetSamples = std::max(64, params.samples * static_cast<int>(files.size()));
    int breakCount = std::max(0, static_cast<int>(flattened.size()) - 1);
    int penUpSamples = breakCount > 0 ? std::max(0, std::min(8, targetSamples / std::max(1, breakCount) / 4)) : 0;
    int strokeSamples = std::max(2, targetSamples - penUpSamples * breakCount);
    vector<int> counts = allocateSampleCountsV2Dft(lengths, strokeSamples, params.minLoopSamples);

    V2DftLoop fullLoop;
    fullLoop.file = channel + "_full_svg";
    fullLoop.loopIndex = 0;
    fullLoop.length = totalLength;
    fullLoop.start = 0.0;
    fullLoop.end = 1.0;
    fullLoop.showArms = true;

    double penUpDistance = 0.0;
    size_t filteredIndex = 0;
    for (size_t i = 0; i < flattened.size(); ++i) {
        if (flattened[i].length < params.minLoopLength) continue;
        if (filteredIndex >= counts.size() || counts[filteredIndex] <= 0) {
            ++filteredIndex;
            continue;
        }
        vector<V2DftPoint> sampled = resampleFlattenedLoopV2Dft(flattened[i], counts[filteredIndex++]);
        sampled = smoothPolylineV2Dft(sampled, params.smoothPasses, isClosedFlattenedLoopV2Dft(flattened[i]));
        if (sampled.empty()) continue;

        if (!fullLoop.points.empty()) {
            V2DftPoint from = fullLoop.points.back();
            V2DftPoint to = sampled.front();
            penUpDistance += distanceV2Dft(from, to);
            for (int p = 1; p <= penUpSamples; ++p) {
                double t = static_cast<double>(p) / static_cast<double>(penUpSamples + 1);
                fullLoop.breaks.push_back(static_cast<int>(fullLoop.points.size()));
                fullLoop.points.push_back({
                    from.x + (to.x - from.x) * t,
                    from.y + (to.y - from.y) * t
                });
            }
            fullLoop.breaks.push_back(static_cast<int>(fullLoop.points.size()));
        }

        fullLoop.points.insert(fullLoop.points.end(), sampled.begin(), sampled.end());
    }

    if (fullLoop.points.size() < 2) {
        cout << "  Not enough samples for full_svg " << image << " / " << channel << "\n";
        return false;
    }

    V2DftSceneParams effectiveParams = params;
    effectiveParams.samples = static_cast<int>(fullLoop.points.size());
    effectiveParams.arms = std::min(params.arms, std::max(1, effectiveParams.samples - 1));
    fullLoop.coeffs = computeDftCoefficientsV2Dft(fullLoop.points, effectiveParams.arms, effectiveParams.coefficientOrder);
    fullLoop.trace = buildTraceV2Dft(static_cast<int>(fullLoop.points.size()), fullLoop.coeffs);

    vector<V2DftLoop> loops;
    loops.push_back(std::move(fullLoop));
    map<string, vector<size_t>> groupsByFile;
    groupsByFile[loops[0].file].push_back(0);

    string dataDir = resultDir + "\\dft_data";
    ensureDirectoryV2Dft(dataDir);
    string outputPath = dataDir + "\\" + channel + "_" + mode + ".json";
    bool ok = writeDftSceneJsonV2Dft(outputPath, image, channel, mode, effectiveParams, loops, width, height);
    if (writeCoefficientFiles) {
        ok = writeFourierCoefficientFilesV2Dft(resultDir, image, channel, mode, effectiveParams, loops, groupsByFile) && ok;
    }

    cout << "  " << channel << " " << mode << ": 1 full-svg loop, base samples=" << params.samples
         << ", components=" << files.size() << ", effective samples=" << effectiveParams.samples
         << ", arms=" << effectiveParams.arms << ", pen-up=" << penUpSamples
         << ", order=" << params.fullSvgOrder << ", coefficient order=" << params.coefficientOrder
         << ", min loop samples=" << params.minLoopSamples
         << ", min loop length=" << params.minLoopLength
         << ", smooth passes=" << params.smoothPasses
         << ", pen-up distance=" << fixed << setprecision(1) << penUpDistance
         << ", output=" << outputPath << "\n";
    return ok;
}

static bool precomputeDftSceneV2(const string& resultDir, const string& image, const string& channel,
                                 const string& mode, const V2DftSceneParams& params,
                                 bool writeCoefficientFiles) {
    string compDir = resultDir + "\\comp";
    vector<string> files = listComponentSvgsV2Dft(compDir, channel);
    if (files.empty()) {
        cout << "  No component SVGs for " << image << " / " << channel << "\n";
        return false;
    }

    vector<V2DftLoop> loops;
    map<string, vector<size_t>> groupsByFile;
    double width = 0.0, height = 0.0;
    size_t totalPointCount = 0;

    for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const string& file = files[fileIndex];
        cout << "    [" << (fileIndex + 1) << "/" << files.size() << "] " << file << "...\n";
        cout.flush();

        string svgPath = compDir + "\\" + file;
        string svgText = readTextFileV2Dft(svgPath);
        if (svgText.empty()) continue;
        if (width <= 0.0 || height <= 0.0) parseViewBoxSizeV2Dft(svgText, width, height);

        V2DftMatrix transform = parseTransformV2Dft(svgText);
        vector<string> pathData = extractPathDataV2Dft(svgText);
        vector<vector<V2DftPoint>> flattened;
        double totalLength = 0.0;
        for (const string& d : pathData) {
            vector<vector<V2DftPoint>> pathLoops = flattenPathV2Dft(d, transform);
            for (auto& loop : pathLoops) {
                double length = polylineLengthV2Dft(loop);
                if (length > 0.0 && length >= params.minLoopLength) {
                    vector<vector<V2DftPoint>> parts = splitPolylineByLengthV2Dft(loop, params.maxLoopLength);
                    for (auto& part : parts) {
                        double partLength = polylineLengthV2Dft(part);
                        if (partLength > 0.0 && partLength >= params.minLoopLength) {
                            totalLength += partLength;
                            flattened.push_back(std::move(part));
                        }
                    }
                }
            }
        }

        int loopIndex = 0;
        size_t filePointCount = 0;
        for (const auto& flattenedLoop : flattened) {
            double length = polylineLengthV2Dft(flattenedLoop);
            if (length < params.minLoopLength) continue;
            int proportionalSamples = static_cast<int>(round(params.samples * length / std::max(1.0, totalLength)));
            int sampleCount = std::max(params.minLoopSamples, proportionalSamples);
            sampleCount = std::min(params.samples, sampleCount);
            vector<V2DftPoint> sampled = resamplePolylineV2Dft(flattenedLoop, sampleCount);
            sampled = smoothPolylineV2Dft(sampled, params.smoothPasses, isClosedPolylineV2Dft(flattenedLoop));
            if (sampled.size() < 2) continue;

            V2DftLoop loop;
            loop.file = file;
            loop.loopIndex = loopIndex++;
            loop.length = polylineLengthV2Dft(sampled);
            loop.points = sampled;
            loop.coeffs = computeDftCoefficientsV2Dft(loop.points, params.arms, params.coefficientOrder);
            loop.trace = buildTraceV2Dft(static_cast<int>(loop.points.size()), loop.coeffs);
            groupsByFile[file].push_back(loops.size());
            filePointCount += loop.points.size();
            totalPointCount += loop.points.size();
            loops.push_back(std::move(loop));
        }

        cout << "      " << loopIndex << " loop(s), points=" << filePointCount
             << ", total loops=" << loops.size() << ", total points=" << totalPointCount << "\n";
    }

    vector<pair<string, double>> groupLengths;
    for (const auto& pair : groupsByFile) {
        double length = 0.0;
        for (size_t idx : pair.second) length += loops[idx].length;
        groupLengths.push_back({pair.first, length});
    }
    sort(groupLengths.begin(), groupLengths.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    double totalGroupWeight = 0.0;
    for (const auto& group : groupLengths) {
        totalGroupWeight += pow(std::max(1.0, group.second), params.componentTimePower);
    }
    totalGroupWeight = std::max(1.0, totalGroupWeight);

    double groupCursor = 0.0;
    for (size_t groupIndex = 0; groupIndex < groupLengths.size(); ++groupIndex) {
        const string& file = groupLengths[groupIndex].first;
        double groupSpan = pow(std::max(1.0, groupLengths[groupIndex].second), params.componentTimePower) / totalGroupWeight;
        double cursor = groupCursor;
        double groupEnd = groupIndex + 1 == groupLengths.size() ? 1.0 : std::min(1.0, groupCursor + groupSpan);
        groupCursor = groupEnd;
        const vector<size_t>& indices = groupsByFile[file];
        for (size_t idx : indices) {
            double localSpan = groupSpan * (loops[idx].length / std::max(1.0, groupLengths[groupIndex].second));
            loops[idx].start = cursor;
            cursor += localSpan;
            loops[idx].end = std::min(groupEnd, cursor);
        }
        if (!indices.empty()) loops[indices.back()].end = groupEnd;
    }

    vector<size_t> byLength(loops.size());
    for (size_t i = 0; i < loops.size(); ++i) byLength[i] = i;
    sort(byLength.begin(), byLength.end(), [&](size_t a, size_t b) { return loops[a].length > loops[b].length; });
    for (int i = 0; i < params.simArmParts && i < static_cast<int>(byLength.size()); ++i) {
        loops[byLength[i]].showArms = true;
    }

    string dataDir = resultDir + "\\dft_data";
    ensureDirectoryV2Dft(dataDir);
    string outputPath = dataDir + "\\" + channel + "_" + mode + ".json";
    bool ok = writeDftSceneJsonV2Dft(outputPath, image, channel, mode, params, loops, width, height);
    if (writeCoefficientFiles) {
        ok = writeFourierCoefficientFilesV2Dft(resultDir, image, channel, mode, params, loops, groupsByFile) && ok;
    }
    cout << "  " << channel << " " << mode << ": " << loops.size()
         << " loop(s), samples=" << params.samples << ", arms=" << params.arms
         << ", min loop samples=" << params.minLoopSamples
         << ", min loop length=" << params.minLoopLength
         << ", max loop length=" << params.maxLoopLength
         << ", smooth passes=" << params.smoothPasses
         << ", output=" << outputPath << "\n";
    if (writeCoefficientFiles) {
        cout << "    coefficient output=" << resultDir << "\\FourierCoefficient\n";
    }
    return ok;
}

static bool precomputeDftForImageV2(const string& outputRoot, const string& imageName,
                                    const vector<string>& channels, const vector<string>& modes,
                                    const map<string, string>& config) {
    string resultDir = outputRoot + "\\" + imageName;
    string compDir = resultDir + "\\comp";
    if (_access(compDir.c_str(), 0) != 0) {
        cout << "Missing comp folder: " << compDir << "\n";
        return false;
    }

    bool ok = true;
    cout << "\nDFT precompute: " << imageName << "\n";
    string coefficientMode = find(modes.begin(), modes.end(), "sequential") != modes.end()
                                ? "sequential"
                                : (modes.empty() ? "" : modes.front());
    for (const string& channel : channels) {
        for (const string& mode : modes) {
            V2DftSceneParams params = paramsForModeV2Dft(config, mode);
            bool writeCoefficientFiles = mode == coefficientMode;
            if (mode == "full_svg") {
                ok = precomputeFullSvgDftSceneV2(resultDir, imageName, channel, mode, params, writeCoefficientFiles) && ok;
            } else {
                ok = precomputeDftSceneV2(resultDir, imageName, channel, mode, params, writeCoefficientFiles) && ok;
            }
        }
    }
    return ok;
}
