#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <direct.h>
#include <io.h>

#include "v2_component_export.h"
#include "v2_dft_precompute.h"

using namespace cv;
using namespace std;

struct ApiLineConfig {
    string provider = "openai_compatible";
    string baseUrl;
    string endpoint = "/images/edits";
    string imageGenerateEndpoint = "/images/generations";
    string responsesEndpoint = "/responses";
    string chatEndpoint = "/chat/completions";
    string apiKey;
    string apiKeyEnv;
    string authHeader = "Authorization";
    string authPrefix = "Bearer ";
    string model;
    string responseModel;
    string prompt = "Extract a clean black-and-white line drawing from the input image. Preserve the main contours and important internal lines. Use a white background and black lines only.";
    string secondaryPrompt;
    string size = "1024x1024";
    string quality;
    string responsesAction = "edit";
    bool stream = true;
    string responseFormat = "b64_json";
    string imageField = "image";
    string responseBase64Key = "b64_json";
    string responseUrlKey = "url";
    int imageCount = 1;
    int timeoutSeconds = 180;
    int thresholdValue = 210;
    bool useOtsu = true;
    bool darkLinesOnLight = true;
    int whiteTolerance = 0;
    int minComponentArea = 3;
    bool runDft = true;
    string dftModes = "all";
    string primaryChannel = "API_Line";
    vector<string> channels = {"API_Line"};
    vector<string> providerOrder = {"responses_image", "openai_compatible", "chat_image"};
    map<string, string> formFields;
    map<string, string> headers;
};

struct CliOptions {
    string configPath = "api_line_config.ini";
    string outputRoot = "results_api";
    string dftConfigPath;
    string dftModesOverride;
    string channelsOverride;
    string localLineImage;
    bool skipApi = false;
    bool precomputeOnly = false;
    bool runDft = true;
    vector<string> imagePaths;
};

struct HttpResponse {
    DWORD status = 0;
    vector<unsigned char> body;
    string error;
};

struct ApiAttempt {
    string provider;
    string endpoint;
    string description;
};

using HttpChunkCallback = function<bool(const unsigned char*, DWORD, string&)>;

static string trimCopy(string text) {
    while (!text.empty() && isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

static string lowerCopy(string text) {
    transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    return text;
}

static bool fileExists(const string& path) {
    return !path.empty() && _access(path.c_str(), 0) == 0;
}

static void ensureDirectory(const string& path) {
    if (!path.empty()) _mkdir(path.c_str());
}

static string pathStem(const string& path) {
    size_t slash = path.find_last_of("\\/");
    size_t start = slash == string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == string::npos || dot < start) dot = path.size();
    return path.substr(start, dot - start);
}

static string pathExtensionLower(const string& path) {
    size_t slash = path.find_last_of("\\/");
    size_t dot = path.find_last_of('.');
    if (dot == string::npos || (slash != string::npos && dot < slash)) return "";
    return lowerCopy(path.substr(dot + 1));
}

static vector<string> splitCsv(const string& text) {
    vector<string> values;
    string current;
    for (char ch : text) {
        if (ch == ',') {
            current = trimCopy(current);
            if (!current.empty()) values.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    current = trimCopy(current);
    if (!current.empty()) values.push_back(current);
    return values;
}

static vector<string> dftModesFromText(const string& text) {
    string value = lowerCopy(trimCopy(text));
    if (value == "both") return {"sequential", "simultaneous"};
    if (value == "all") return {"sequential", "simultaneous", "full_svg"};

    vector<string> parsed = splitCsv(value);
    vector<string> modes;
    for (const string& mode : parsed) {
        if (mode == "sequential" || mode == "simultaneous" || mode == "full_svg") {
            modes.push_back(mode);
        }
    }
    return modes.empty() ? vector<string>{"sequential", "simultaneous", "full_svg"} : modes;
}

static vector<string> splitPipeList(const string& text) {
    vector<string> values;
    string current;
    for (char ch : text) {
        if (ch == '|' || ch == ',') {
            current = trimCopy(current);
            if (!current.empty()) values.push_back(lowerCopy(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    current = trimCopy(current);
    if (!current.empty()) values.push_back(lowerCopy(current));
    return values;
}
static map<string, string> readKeyValueFile(const string& path) {
    map<string, string> config;
    ifstream in(path, ios::in | ios::binary);
    if (!in) return config;

    string line;
    while (getline(in, line)) {
        size_t hash = line.find('#');
        size_t semi = line.find(';');
        size_t comment = string::npos;
        if (hash != string::npos) comment = hash;
        if (semi != string::npos) comment = comment == string::npos ? semi : std::min(comment, semi);
        if (comment != string::npos) line = line.substr(0, comment);

        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string key = trimCopy(line.substr(0, eq));
        string value = trimCopy(line.substr(eq + 1));
        if (!key.empty()) config[key] = value;
    }
    return config;
}

static string getConfigString(const map<string, string>& raw, const string& key, const string& fallback = "") {
    auto it = raw.find(key);
    return it == raw.end() ? fallback : it->second;
}

static int getConfigInt(const map<string, string>& raw, const string& key, int fallback, int minValue, int maxValue) {
    string text = getConfigString(raw, key, "");
    if (text.empty()) return fallback;
    int value = atoi(text.c_str());
    return std::max(minValue, std::min(maxValue, value));
}

static bool getConfigBool(const map<string, string>& raw, const string& key, bool fallback) {
    string value = lowerCopy(getConfigString(raw, key, ""));
    if (value.empty()) return fallback;
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

static bool startsWith(const string& text, const string& prefix) {
    return text.size() >= prefix.size() && equal(prefix.begin(), prefix.end(), text.begin());
}

static string getEnvString(const string& name) {
    if (name.empty()) return "";
    DWORD needed = GetEnvironmentVariableA(name.c_str(), nullptr, 0);
    if (needed == 0) return "";
    string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(name.c_str(), value.data(), needed);
    if (written == 0) return "";
    value.resize(written);
    return value;
}

static ApiLineConfig loadApiLineConfig(const string& path) {
    map<string, string> raw = readKeyValueFile(path);
    ApiLineConfig config;

    config.provider = lowerCopy(getConfigString(raw, "provider", config.provider));
    config.baseUrl = getConfigString(raw, "base_url", config.baseUrl);
    config.endpoint = getConfigString(raw, "endpoint", config.endpoint);
    config.imageGenerateEndpoint = getConfigString(raw, "image_generate_endpoint", config.imageGenerateEndpoint);
    config.responsesEndpoint = getConfigString(raw, "responses_endpoint", config.responsesEndpoint);
    config.chatEndpoint = getConfigString(raw, "chat_endpoint", config.chatEndpoint);
    config.apiKey = getConfigString(raw, "api_key", config.apiKey);
    config.apiKeyEnv = getConfigString(raw, "api_key_env", config.apiKeyEnv);
    config.authHeader = getConfigString(raw, "auth_header", config.authHeader);
    config.authPrefix = getConfigString(raw, "auth_prefix", config.authPrefix);
    config.model = getConfigString(raw, "model", config.model);
    config.responseModel = getConfigString(raw, "response_model", config.responseModel);
    config.prompt = getConfigString(raw, "prompt", config.prompt);
    config.secondaryPrompt = getConfigString(raw, "prompt_2", getConfigString(raw, "secondary_prompt", config.secondaryPrompt));
    config.size = getConfigString(raw, "size", config.size);
    config.quality = getConfigString(raw, "quality", config.quality);
    config.responsesAction = getConfigString(raw, "responses_action", config.responsesAction);
    config.stream = getConfigBool(raw, "stream", config.stream);
    config.responseFormat = getConfigString(raw, "response_format", config.responseFormat);
    config.imageField = getConfigString(raw, "image_field", config.imageField);
    config.responseBase64Key = getConfigString(raw, "response_base64_key", config.responseBase64Key);
    config.responseUrlKey = getConfigString(raw, "response_url_key", config.responseUrlKey);
    config.imageCount = getConfigInt(raw, "image_count", getConfigInt(raw, "n", config.imageCount, 1, 8), 1, 8);
    config.timeoutSeconds = getConfigInt(raw, "timeout_seconds", config.timeoutSeconds, 10, 3600);
    config.thresholdValue = getConfigInt(raw, "threshold", config.thresholdValue, 0, 255);
    config.useOtsu = getConfigBool(raw, "use_otsu", config.useOtsu);
    config.darkLinesOnLight = lowerCopy(getConfigString(raw, "line_polarity", "dark_on_light")) != "light_on_dark";
    config.whiteTolerance = getConfigInt(raw, "white_tolerance", config.whiteTolerance, 0, 255);
    config.minComponentArea = getConfigInt(raw, "min_component_area", config.minComponentArea, 1, 1000000);
    config.runDft = getConfigBool(raw, "run_dft", config.runDft);
    config.dftModes = getConfigString(raw, "dft_modes", config.dftModes);
    config.primaryChannel = getConfigString(raw, "primary_channel", config.primaryChannel);
    string channels = getConfigString(raw, "channels", "");
    if (!channels.empty()) config.channels = splitCsv(channels);
    string providerOrder = getConfigString(raw, "provider_order", "");
    if (!providerOrder.empty()) config.providerOrder = splitPipeList(providerOrder);

    if (config.apiKey.empty() && !config.apiKeyEnv.empty()) {
        config.apiKey = getEnvString(config.apiKeyEnv);
    }

    for (const auto& pair : raw) {
        if (startsWith(pair.first, "form.")) {
            config.formFields[pair.first.substr(5)] = pair.second;
        } else if (startsWith(pair.first, "header.")) {
            config.headers[pair.first.substr(7)] = pair.second;
        }
    }

    return config;
}

static bool isPlaceholderKey(const string& key) {
    string value = lowerCopy(trimCopy(key));
    return value.empty() || value == "sk-xxxx" || value == "sk-xxxxxxxx" ||
           value == "your_api_key_here" || value == "replace_me";
}

static bool isPlaceholderModel(const string& model) {
    string value = lowerCopy(trimCopy(model));
    return value.empty() || value == "your-image-edit-model" ||
           value == "your_image_edit_model" || value == "replace_me";
}

static bool hasFormField(const ApiLineConfig& config, const string& name) {
    return config.formFields.find(name) != config.formFields.end();
}

static bool modelReturnsBase64ByDefault(const string& model) {
    return startsWith(lowerCopy(model), "gpt-image");
}

static string resolveRequestedImageSize(const ApiLineConfig& config,
                                        const vector<unsigned char>& inputBytes) {
    string requested = lowerCopy(trimCopy(config.size));
    if (requested.empty()) return "";
    if (requested != "auto" && requested != "adaptive" && requested != "preserve_aspect") {
        return config.size;
    }

    Mat input = imdecode(inputBytes, IMREAD_UNCHANGED);
    if (input.empty() || input.cols <= 0 || input.rows <= 0) {
        return "1024x1024";
    }

    double aspect = static_cast<double>(input.cols) / static_cast<double>(input.rows);
    if (aspect > 1.25) return "1536x1024";
    if (aspect < 0.8) return "1024x1536";
    return "1024x1024";
}

static bool shouldRestoreInputCanvas(const ApiLineConfig& config) {
    string requested = lowerCopy(trimCopy(config.size));
    return requested == "auto" || requested == "adaptive" || requested == "preserve_aspect";
}

static Scalar canvasBackgroundForImage(const Mat& image) {
    if (image.channels() == 4) return Scalar(255, 255, 255, 0);
    if (image.channels() == 3) return Scalar(255, 255, 255);
    return Scalar(255);
}

static Mat fitImageOnCanvasPreserveAspect(const Mat& image, const Size& canvasSize) {
    if (image.empty() || canvasSize.width <= 0 || canvasSize.height <= 0) return image.clone();
    if (image.cols <= 0 || image.rows <= 0) return image.clone();

    double scale = min(static_cast<double>(canvasSize.width) / static_cast<double>(image.cols),
                       static_cast<double>(canvasSize.height) / static_cast<double>(image.rows));
    int fittedWidth = max(1, static_cast<int>(round(image.cols * scale)));
    int fittedHeight = max(1, static_cast<int>(round(image.rows * scale)));
    if (fittedWidth > canvasSize.width) fittedWidth = canvasSize.width;
    if (fittedHeight > canvasSize.height) fittedHeight = canvasSize.height;

    Mat fitted;
    resize(image, fitted, Size(fittedWidth, fittedHeight), 0, 0, INTER_LANCZOS4);

    Mat canvas(canvasSize, fitted.type(), canvasBackgroundForImage(fitted));
    int x = (canvasSize.width - fittedWidth) / 2;
    int y = (canvasSize.height - fittedHeight) / 2;
    fitted.copyTo(canvas(Rect(x, y, fittedWidth, fittedHeight)));
    return canvas;
}

static Size readImageSize(const string& imagePath) {
    Mat image = imread(imagePath, IMREAD_UNCHANGED);
    if (image.empty()) return Size();
    return image.size();
}

static wstring toWideUtf8(const string& text) {
    if (text.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        needed = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0) return L"";
        wstring result(needed, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), result.data(), needed);
        return result;
    }
    wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), needed);
    return result;
}

static string lastWindowsErrorText(const string& prefix) {
    DWORD code = GetLastError();
    ostringstream stream;
    stream << prefix << " (Win32 error " << code << ")";
    return stream.str();
}

static bool readBinaryFile(const string& path, vector<unsigned char>& bytes) {
    ifstream in(path, ios::in | ios::binary);
    if (!in) return false;
    in.seekg(0, ios::end);
    streamoff size = in.tellg();
    in.seekg(0, ios::beg);
    if (size < 0) return false;
    bytes.resize(static_cast<size_t>(size));
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    return true;
}

static bool writeBinaryFile(const string& path, const vector<unsigned char>& bytes) {
    ofstream out(path, ios::out | ios::binary);
    if (!out) return false;
    if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return static_cast<bool>(out);
}

static HttpResponse httpRequest(const string& url,
                                const string& method,
                                const vector<unsigned char>& requestBody,
                                const vector<pair<string, string>>& headers,
                                int timeoutSeconds,
                                const HttpChunkCallback& onChunk = nullptr) {
    HttpResponse response;
    wstring wideUrl = toWideUtf8(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        response.error = lastWindowsErrorText("Invalid URL");
        return response;
    }

    wstring host(parts.lpszHostName, parts.dwHostNameLength);
    wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    HINTERNET session = WinHttpOpen(L"FourierApiApproach/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        response.error = lastWindowsErrorText("WinHttpOpen failed");
        return response;
    }

    int timeoutMs = std::max(1, timeoutSeconds) * 1000;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        response.error = lastWindowsErrorText("WinHttpConnect failed");
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    wstring wideMethod = toWideUtf8(method);
    HINTERNET request = WinHttpOpenRequest(connect,
                                           wideMethod.c_str(),
                                           path.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags);
    if (!request) {
        response.error = lastWindowsErrorText("WinHttpOpenRequest failed");
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }

    wstring headerText;
    for (const auto& header : headers) {
        if (header.first.empty()) continue;
        headerText += toWideUtf8(header.first + ": " + header.second + "\r\n");
    }

    LPVOID bodyPtr = requestBody.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<unsigned char*>(requestBody.data());
    DWORD bodySize = static_cast<DWORD>(requestBody.size());
    BOOL ok = WinHttpSendRequest(request,
                                 headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                                 headerText.empty() ? 0 : static_cast<DWORD>(-1),
                                 bodyPtr,
                                 bodySize,
                                 bodySize,
                                 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
        response.error = lastWindowsErrorText("HTTP request failed");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    response.status = status;

    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            response.error = lastWindowsErrorText("WinHttpQueryDataAvailable failed");
            break;
        }
        if (available == 0) break;

        vector<unsigned char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            response.error = lastWindowsErrorText("WinHttpReadData failed");
            break;
        }
        if (read == 0) continue;
        response.body.insert(response.body.end(), chunk.begin(), chunk.begin() + read);
        if (onChunk) {
            string callbackError;
            if (onChunk(chunk.data(), read, callbackError)) {
                break;
            }
            if (!callbackError.empty()) {
                response.error = callbackError;
                break;
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

static string joinApiUrl(const string& baseUrl, const string& endpoint) {
    if (startsWith(lowerCopy(endpoint), "http://") || startsWith(lowerCopy(endpoint), "https://")) {
        return endpoint;
    }
    string base = baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    string path = endpoint.empty() ? "" : endpoint;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    return base + "/" + path;
}

static string guessMimeType(const string& path) {
    string ext = pathExtensionLower(path);
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "bmp") return "image/bmp";
    if (ext == "webp") return "image/webp";
    return "image/png";
}

static void appendText(vector<unsigned char>& body, const string& text) {
    body.insert(body.end(), text.begin(), text.end());
}

static vector<unsigned char> buildMultipartBody(const string& boundary,
                                                const vector<pair<string, string>>& fields,
                                                const string& fileField,
                                                const string& filePath,
                                                const vector<unsigned char>& fileBytes,
                                                const string& mimeType) {
    vector<unsigned char> body;
    for (const auto& field : fields) {
        if (field.first.empty() || field.second.empty()) continue;
        appendText(body, "--" + boundary + "\r\n");
        appendText(body, "Content-Disposition: form-data; name=\"" + field.first + "\"\r\n\r\n");
        appendText(body, field.second + "\r\n");
    }

    string fileName = pathStem(filePath);
    string ext = pathExtensionLower(filePath);
    if (!ext.empty()) fileName += "." + ext;

    appendText(body, "--" + boundary + "\r\n");
    appendText(body, "Content-Disposition: form-data; name=\"" + fileField +
                     "\"; filename=\"" + fileName + "\"\r\n");
    appendText(body, "Content-Type: " + mimeType + "\r\n\r\n");
    body.insert(body.end(), fileBytes.begin(), fileBytes.end());
    appendText(body, "\r\n--" + boundary + "--\r\n");
    return body;
}

static bool parseJsonStringAt(const string& text, size_t quotePos, string& value) {
    if (quotePos == string::npos || quotePos >= text.size() || text[quotePos] != '"') return false;
    value.clear();
    for (size_t i = quotePos + 1; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '"') return true;
        if (ch == '\\' && i + 1 < text.size()) {
            char esc = text[++i];
            if (esc == 'n') value.push_back('\n');
            else if (esc == 'r') value.push_back('\r');
            else if (esc == 't') value.push_back('\t');
            else if (esc == '"' || esc == '\\' || esc == '/') value.push_back(esc);
            else value.push_back(esc);
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

static bool extractJsonStringByKey(const string& text, const string& key, string& value) {
    if (key.empty()) return false;
    string quotedKey = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = text.find(quotedKey, pos)) != string::npos) {
        size_t colon = text.find(':', pos + quotedKey.size());
        if (colon == string::npos) return false;
        size_t quote = colon + 1;
        while (quote < text.size() && isspace(static_cast<unsigned char>(text[quote]))) ++quote;
        if (quote < text.size() && text[quote] == '"' && parseJsonStringAt(text, quote, value)) {
            return true;
        }
        pos = colon + 1;
    }
    return false;
}

static vector<string> extractJsonStringsByKey(const string& text, const string& key) {
    vector<string> values;
    if (key.empty()) return values;
    string quotedKey = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = text.find(quotedKey, pos)) != string::npos) {
        size_t colon = text.find(':', pos + quotedKey.size());
        if (colon == string::npos) break;
        size_t quote = colon + 1;
        while (quote < text.size() && isspace(static_cast<unsigned char>(text[quote]))) ++quote;
        string value;
        if (quote < text.size() && text[quote] == '"' && parseJsonStringAt(text, quote, value)) {
            values.push_back(value);
            pos = quote + 1;
        } else {
            pos = colon + 1;
        }
    }
    return values;
}

static bool extractFirstMarkdownImageUrl(const string& text, string& imageUrl) {
    size_t bang = text.find("![");
    while (bang != string::npos) {
        size_t open = text.find("](", bang);
        if (open == string::npos) break;
        size_t close = text.find(')', open + 2);
        if (close == string::npos) break;
        imageUrl = text.substr(open + 2, close - (open + 2));
        imageUrl = trimCopy(imageUrl);
        return !imageUrl.empty();
    }
    return false;
}

static string stripDataUrlPrefix(string base64Text) {
    size_t comma = base64Text.find(',');
    if (startsWith(lowerCopy(base64Text), "data:") && comma != string::npos) {
        return base64Text.substr(comma + 1);
    }
    return base64Text;
}

static bool decodeBase64(const string& base64Text, vector<unsigned char>& bytes) {
    string clean = stripDataUrlPrefix(base64Text);
    DWORD needed = 0;
    if (!CryptStringToBinaryA(clean.c_str(),
                              static_cast<DWORD>(clean.size()),
                              CRYPT_STRING_BASE64,
                              nullptr,
                              &needed,
                              nullptr,
                              nullptr)) {
        return false;
    }
    bytes.resize(needed);
    return CryptStringToBinaryA(clean.c_str(),
                                static_cast<DWORD>(clean.size()),
                                CRYPT_STRING_BASE64,
                                bytes.data(),
                                &needed,
                                nullptr,
                                nullptr) != FALSE;
}

static bool encodeBase64(const vector<unsigned char>& bytes, string& base64Text) {
    base64Text.clear();
    if (bytes.empty()) return true;

    DWORD needed = 0;
    if (!CryptBinaryToStringA(bytes.data(),
                              static_cast<DWORD>(bytes.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr,
                              &needed)) {
        return false;
    }
    string encoded(needed, '\0');
    if (!CryptBinaryToStringA(bytes.data(),
                              static_cast<DWORD>(bytes.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              encoded.data(),
                              &needed)) {
        return false;
    }
    if (!encoded.empty() && encoded.back() == '\0') encoded.pop_back();
    base64Text = std::move(encoded);
    return true;
}

static string jsonEscape(const string& text) {
    string out;
    for (char ch : text) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else out.push_back(ch);
    }
    return out;
}

static string responseTextPreview(const vector<unsigned char>& body) {
    size_t count = std::min<size_t>(body.size(), 1600);
    string text(body.begin(), body.begin() + count);
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    return text;
}

static bool extractImageBytesFromSseBuffer(const ApiLineConfig& config,
                                           const string& sseText,
                                           vector<unsigned char>& imageBytes,
                                           string& error);

static bool extractImageBytesFromResponse(const ApiLineConfig& config,
                                          const vector<unsigned char>& responseBody,
                                          vector<unsigned char>& imageBytes,
                                          string& error) {
    string text(responseBody.begin(), responseBody.end());
    string base64Text;
    vector<string> base64Keys = {config.responseBase64Key, "b64_json", "result", "image_base64", "base64"};
    for (const string& key : base64Keys) {
        if (extractJsonStringByKey(text, key, base64Text) && !base64Text.empty()) {
            if (decodeBase64(base64Text, imageBytes)) return true;
            error = "Response contained key '" + key + "', but base64 decoding failed.";
            return false;
        }
    }

    string imageUrl;
    vector<string> urlKeys = {config.responseUrlKey, "url"};
    for (const string& key : urlKeys) {
        if (extractJsonStringByKey(text, key, imageUrl) && !imageUrl.empty()) {
            if (startsWith(lowerCopy(imageUrl), "data:")) {
                if (decodeBase64(imageUrl, imageBytes)) return true;
                error = "Response contained a data URL, but base64 decoding failed.";
                return false;
            }

            HttpResponse download = httpRequest(imageUrl, "GET", {}, {}, config.timeoutSeconds);
            if (!download.error.empty()) {
                error = "Could not download response image: " + download.error;
                return false;
            }
            if (download.status < 200 || download.status >= 300) {
                error = "Image URL returned HTTP " + to_string(download.status);
                return false;
            }
            imageBytes = std::move(download.body);
            return true;
        }
    }

    error = "Could not find a base64 image or image URL in the API response. Response preview: " +
            responseTextPreview(responseBody);
    return false;
}

static bool extractImageListFromResponse(const ApiLineConfig& config,
                                         const vector<unsigned char>& responseBody,
                                         vector<vector<unsigned char>>& images,
                                         string& error) {
    images.clear();
    string text(responseBody.begin(), responseBody.end());
    vector<string> base64Keys = {config.responseBase64Key, "b64_json", "result", "image_base64", "base64"};
    for (const string& key : base64Keys) {
        vector<string> values = extractJsonStringsByKey(text, key);
        for (const string& value : values) {
            vector<unsigned char> bytes;
            if (decodeBase64(value, bytes) && !bytes.empty()) {
                images.push_back(std::move(bytes));
            }
        }
        if (!images.empty()) return true;
    }

    vector<string> urlKeys = {config.responseUrlKey, "url", "image_url"};
    for (const string& key : urlKeys) {
        vector<string> values = extractJsonStringsByKey(text, key);
        for (const string& value : values) {
            if (value.empty()) continue;
            vector<unsigned char> bytes;
            if (startsWith(lowerCopy(value), "data:")) {
                if (decodeBase64(value, bytes) && !bytes.empty()) {
                    images.push_back(std::move(bytes));
                }
                continue;
            }

            HttpResponse download = httpRequest(value, "GET", {}, {}, config.timeoutSeconds);
            if (download.error.empty() && download.status >= 200 && download.status < 300 && !download.body.empty()) {
                images.push_back(std::move(download.body));
            }
        }
        if (!images.empty()) return true;
    }

    error = "Could not find images in the API response. Response preview: " + responseTextPreview(responseBody);
    return false;
}

static bool callOpenAiCompatibleImageEditImages(const ApiLineConfig& config,
                                                const string& imagePath,
                                                vector<vector<unsigned char>>& images,
                                                string& error) {
    if (config.baseUrl.empty() && !startsWith(lowerCopy(config.endpoint), "http")) {
        error = "base_url is empty.";
        return false;
    }
    if (isPlaceholderKey(config.apiKey)) {
        error = "api_key is empty or still a placeholder. Fill api_line_config.ini or set api_key_env.";
        return false;
    }
    if (config.model.empty()) {
        error = "model is empty.";
        return false;
    }

    vector<unsigned char> inputBytes;
    if (!readBinaryFile(imagePath, inputBytes)) {
        error = "Could not read input image: " + imagePath;
        return false;
    }
    string requestedSize = resolveRequestedImageSize(config, inputBytes);

    vector<pair<string, string>> fields;
    fields.push_back({"model", config.model});
    fields.push_back({"prompt", config.prompt});
    if (!requestedSize.empty()) fields.push_back({"size", requestedSize});
    if (!config.quality.empty() && !hasFormField(config, "quality")) {
        fields.push_back({"quality", config.quality});
    }
    if (config.imageCount > 1 && !hasFormField(config, "n")) {
        fields.push_back({"n", to_string(config.imageCount)});
    }
    if (!config.responseFormat.empty() && !modelReturnsBase64ByDefault(config.model)) {
        fields.push_back({"response_format", config.responseFormat});
    }
    if (config.stream && !hasFormField(config, "stream")) {
        fields.push_back({"stream", "true"});
    }
    if (config.stream && !hasFormField(config, "partial_images")) {
        fields.push_back({"partial_images", "0"});
    }
    for (const auto& field : config.formFields) {
        fields.push_back({field.first, field.second});
    }

    string boundary = "----FourierApiApproachBoundary7MA4YWxkTrZu0gW";
    vector<unsigned char> body = buildMultipartBody(boundary,
                                                    fields,
                                                    config.imageField,
                                                    imagePath,
                                                    inputBytes,
                                                    guessMimeType(imagePath));

    vector<pair<string, string>> headers;
    headers.push_back({"Content-Type", "multipart/form-data; boundary=" + boundary});
    headers.push_back({"Accept", config.stream ? "text/event-stream" : "application/json"});
    if (!config.authHeader.empty()) {
        headers.push_back({config.authHeader, config.authPrefix + config.apiKey});
    }
    for (const auto& header : config.headers) {
        headers.push_back({header.first, header.second});
    }

    string url = joinApiUrl(config.baseUrl, config.endpoint);
    vector<unsigned char> streamedImageBytes;
    bool gotStreamedImage = false;
    string streamBuffer;
    HttpChunkCallback onChunk;
    if (config.stream) {
        onChunk = [&](const unsigned char* data, DWORD size, string& callbackError) {
            streamBuffer.append(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + size);
            string parseError;
            vector<unsigned char> parsedBytes;
            if (extractImageBytesFromSseBuffer(config, streamBuffer, parsedBytes, parseError)) {
                streamedImageBytes = std::move(parsedBytes);
                gotStreamedImage = true;
                return true;
            }
            return false;
        };
    }

    HttpResponse response = httpRequest(url, "POST", body, headers, config.timeoutSeconds, onChunk);
    if (!response.error.empty()) {
        error = response.error;
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        error = "API returned HTTP " + to_string(response.status) + ". Response preview: " +
                responseTextPreview(response.body);
        return false;
    }
    if (gotStreamedImage) {
        images.clear();
        images.push_back(std::move(streamedImageBytes));
        return true;
    }

    return extractImageListFromResponse(config, response.body, images, error);
}

static bool callOpenAiCompatibleImageEditWithPrompt(const ApiLineConfig& config,
                                                    const string& imagePath,
                                                    const string& prompt,
                                                    vector<unsigned char>& imageBytes,
                                                    string& error) {
    ApiLineConfig promptConfig = config;
    promptConfig.prompt = prompt;
    promptConfig.imageCount = 1;
    vector<vector<unsigned char>> images;
    if (!callOpenAiCompatibleImageEditImages(promptConfig, imagePath, images, error)) return false;
    if (images.empty()) {
        error = "API response did not contain any images.";
        return false;
    }
    imageBytes = std::move(images.front());
    return true;
}

static bool callOpenAiCompatibleImageEdit(const ApiLineConfig& config,
                                          const string& imagePath,
                                          vector<unsigned char>& imageBytes,
                                          string& error) {
    vector<vector<unsigned char>> images;
    if (!callOpenAiCompatibleImageEditImages(config, imagePath, images, error)) return false;
    if (images.empty()) {
        error = "API response did not contain any images.";
        return false;
    }
    imageBytes = std::move(images.front());
    return true;
}

static string buildResponsesImageRequestJson(const ApiLineConfig& config,
                                             const string& requestedSize,
                                             const string& imageDataUrl) {
    string model = config.responseModel.empty() ? config.model : config.responseModel;
    string action = lowerCopy(config.responsesAction);
    if (action != "generate" && action != "edit") action = "edit";

    ostringstream json;
    json << "{";
    json << "\"model\":\"" << jsonEscape(model) << "\",";
    json << "\"input\":[{";
    json << "\"role\":\"user\",";
    json << "\"content\":[";
    json << "{";
    json << "\"type\":\"input_text\",";
    json << "\"text\":\"" << jsonEscape(config.prompt) << "\"";
    json << "}";
    if (action == "edit") {
        json << ",{";
        json << "\"type\":\"input_image\",";
        json << "\"image_url\":\"" << jsonEscape(imageDataUrl) << "\"";
        json << "}";
    }
    json << "]";
    json << "}],";
    json << "\"tools\":[{";
    json << "\"type\":\"image_generation\"";
    if (!requestedSize.empty()) {
        json << ",\"size\":\"" << jsonEscape(requestedSize) << "\"";
    }
    if (!config.quality.empty()) {
        json << ",\"quality\":\"" << jsonEscape(config.quality) << "\"";
    }
    json << "}]";
    json << "}";
    return json.str();
}

static bool callOpenAiResponsesImage(const ApiLineConfig& config,
                                     const string& imagePath,
                                     vector<unsigned char>& imageBytes,
                                     string& error) {
    if (config.baseUrl.empty() && !startsWith(lowerCopy(config.responsesEndpoint), "http")) {
        error = "base_url is empty.";
        return false;
    }
    if (isPlaceholderKey(config.apiKey)) {
        error = "api_key is empty or still a placeholder. Fill api_line_config.ini or set api_key_env.";
        return false;
    }
    string model = config.responseModel.empty() ? config.model : config.responseModel;
    if (model.empty()) {
        error = "model is empty.";
        return false;
    }

    vector<unsigned char> inputBytes;
    if (!readBinaryFile(imagePath, inputBytes)) {
        error = "Could not read input image: " + imagePath;
        return false;
    }

    string inputBase64;
    if (!encodeBase64(inputBytes, inputBase64)) {
        error = "Could not base64-encode input image.";
        return false;
    }

    string requestedSize = resolveRequestedImageSize(config, inputBytes);
    string dataUrl = "data:" + guessMimeType(imagePath) + ";base64," + inputBase64;
    string requestJson = buildResponsesImageRequestJson(config, requestedSize, dataUrl);
    vector<unsigned char> body(requestJson.begin(), requestJson.end());

    vector<pair<string, string>> headers;
    headers.push_back({"Content-Type", "application/json"});
    headers.push_back({"Accept", "application/json"});
    if (!config.authHeader.empty()) {
        headers.push_back({config.authHeader, config.authPrefix + config.apiKey});
    }
    for (const auto& header : config.headers) {
        headers.push_back({header.first, header.second});
    }

    string url = joinApiUrl(config.baseUrl, config.responsesEndpoint);
    HttpResponse response = httpRequest(url, "POST", body, headers, config.timeoutSeconds);
    if (!response.error.empty()) {
        error = response.error;
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        error = "API returned HTTP " + to_string(response.status) + ". Response preview: " +
                responseTextPreview(response.body);
        return false;
    }

    return extractImageBytesFromResponse(config, response.body, imageBytes, error);
}

static string buildChatImageRequestJson(const ApiLineConfig& config,
                                        const string& requestedSize,
                                        const string& imageDataUrl) {
    ostringstream json;
    json << "{";
    json << "\"model\":\"" << jsonEscape(config.model) << "\",";
    json << "\"stream\":" << (config.stream ? "true" : "false") << ",";
    json << "\"messages\":[{";
    json << "\"role\":\"user\",";
    json << "\"content\":[";
    json << "{";
    json << "\"type\":\"text\",";
    json << "\"text\":\"" << jsonEscape(config.prompt) << "\"";
    json << "},";
    json << "{";
    json << "\"type\":\"image_url\",";
    json << "\"image_url\":{\"url\":\"" << jsonEscape(imageDataUrl) << "\"}";
    json << "}";
    json << "]";
    json << "}]";
    json << "}";
    return json.str();
}

static string collectTextFromChatResponse(const string& responseText) {
    string collected;
    size_t pos = 0;
    while (true) {
        size_t key = responseText.find("\"content\"", pos);
        if (key == string::npos) break;
        size_t colon = responseText.find(':', key);
        if (colon == string::npos) break;
        size_t quote = colon + 1;
        while (quote < responseText.size() && isspace(static_cast<unsigned char>(responseText[quote]))) ++quote;
        string chunk;
        if (quote < responseText.size() && responseText[quote] == '"' && parseJsonStringAt(responseText, quote, chunk)) {
            collected += chunk;
            pos = quote + chunk.size() + 2;
        } else {
            pos = colon + 1;
        }
    }
    return collected.empty() ? responseText : collected;
}

static bool extractImageBytesFromSseBuffer(const ApiLineConfig& config,
                                           const string& sseText,
                                           vector<unsigned char>& imageBytes,
                                           string& error) {
    string text = collectTextFromChatResponse(sseText);
    string imageUrl;
    if (extractFirstMarkdownImageUrl(text, imageUrl)) {
        if (startsWith(lowerCopy(imageUrl), "data:")) {
            if (decodeBase64(imageUrl, imageBytes)) return true;
            error = "SSE response contained a data URL, but base64 decoding failed.";
            return false;
        }

        HttpResponse download = httpRequest(imageUrl, "GET", {}, {}, config.timeoutSeconds);
        if (!download.error.empty()) {
            error = "Could not download streamed image URL: " + download.error;
            return false;
        }
        if (download.status < 200 || download.status >= 300) {
            error = "Streamed image URL returned HTTP " + to_string(download.status);
            return false;
        }
        imageBytes = std::move(download.body);
        return true;
    }

    return extractImageBytesFromResponse(config,
                                         vector<unsigned char>(sseText.begin(), sseText.end()),
                                         imageBytes,
                                         error);
}

static bool callChatImageStream(const ApiLineConfig& config,
                                const string& imagePath,
                                vector<unsigned char>& imageBytes,
                                string& error) {
    if (config.baseUrl.empty() && !startsWith(lowerCopy(config.chatEndpoint), "http")) {
        error = "base_url is empty.";
        return false;
    }
    if (isPlaceholderKey(config.apiKey)) {
        error = "api_key is empty or still a placeholder. Fill api_line_config.ini or set api_key_env.";
        return false;
    }
    if (config.model.empty()) {
        error = "model is empty.";
        return false;
    }

    vector<unsigned char> inputBytes;
    if (!readBinaryFile(imagePath, inputBytes)) {
        error = "Could not read input image: " + imagePath;
        return false;
    }

    string inputBase64;
    if (!encodeBase64(inputBytes, inputBase64)) {
        error = "Could not base64-encode input image.";
        return false;
    }

    string requestedSize = resolveRequestedImageSize(config, inputBytes);
    string dataUrl = "data:" + guessMimeType(imagePath) + ";base64," + inputBase64;
    string requestJson = buildChatImageRequestJson(config, requestedSize, dataUrl);
    vector<unsigned char> body(requestJson.begin(), requestJson.end());

    vector<pair<string, string>> headers;
    headers.push_back({"Content-Type", "application/json"});
    headers.push_back({"Accept", config.stream ? "text/event-stream" : "application/json"});
    if (!config.authHeader.empty()) {
        headers.push_back({config.authHeader, config.authPrefix + config.apiKey});
    }
    for (const auto& header : config.headers) {
        headers.push_back({header.first, header.second});
    }

    string url = joinApiUrl(config.baseUrl, config.chatEndpoint);
    HttpResponse response = httpRequest(url, "POST", body, headers, config.timeoutSeconds);
    if (!response.error.empty()) {
        error = response.error;
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        error = "API returned HTTP " + to_string(response.status) + ". Response preview: " +
                responseTextPreview(response.body);
        return false;
    }

    string responseText(response.body.begin(), response.body.end());
    string text = collectTextFromChatResponse(responseText);

    string imageUrl;
    if (!extractFirstMarkdownImageUrl(text, imageUrl)) {
        string base64Text;
        if (extractJsonStringByKey(responseText, "b64_json", base64Text) ||
            extractJsonStringByKey(responseText, "image_base64", base64Text) ||
            extractJsonStringByKey(responseText, "base64", base64Text)) {
            if (decodeBase64(base64Text, imageBytes)) return true;
        }
        error = "Could not find a Markdown image URL in chat response. Response preview: " +
                responseTextPreview(response.body);
        return false;
    }

    if (startsWith(lowerCopy(imageUrl), "data:")) {
        if (decodeBase64(imageUrl, imageBytes)) return true;
        error = "Chat response contained a data URL, but base64 decoding failed.";
        return false;
    }

    HttpResponse download = httpRequest(imageUrl, "GET", {}, {}, config.timeoutSeconds);
    if (!download.error.empty()) {
        error = "Could not download chat image URL: " + download.error;
        return false;
    }
    if (download.status < 200 || download.status >= 300) {
        error = "Chat image URL returned HTTP " + to_string(download.status);
        return false;
    }
    imageBytes = std::move(download.body);
    return true;
}

static Mat removeSmallComponentsApi(const Mat& binary, int minArea) {
    if (binary.empty()) return Mat();
    Mat labels, stats, centroids;
    int count = connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    Mat kept = Mat::zeros(binary.size(), CV_8UC1);
    for (int label = 1; label < count; ++label) {
        int area = stats.at<int>(label, CC_STAT_AREA);
        if (area < minArea) continue;
        Mat componentMask;
        compare(labels, label, componentMask, CMP_EQ);
        kept.setTo(255, componentMask);
    }
    return kept;
}

static int chooseAdaptiveBlockSize(const Size& size) {
    int minDim = max(1, min(size.width, size.height));
    int blockSize = minDim / 30;
    blockSize = max(15, min(61, blockSize));
    if (blockSize % 2 == 0) ++blockSize;
    return blockSize;
}

static Mat buildMaskFromApiLineImage(const Mat& image, const ApiLineConfig& config) {
    if (image.empty()) return Mat();

    Mat color;
    Mat alpha;
    if (image.channels() == 4) {
        vector<Mat> planes;
        split(image, planes);
        alpha = planes[3];
        merge(vector<Mat>{planes[0], planes[1], planes[2]}, color);
    } else {
        color = image;
    }

    Mat gray;
    if (color.channels() == 3) {
        cvtColor(color, gray, COLOR_BGR2GRAY);
    } else {
        gray = color.clone();
    }
    if (gray.type() != CV_8UC1) {
        normalize(gray, gray, 0, 255, NORM_MINMAX);
        gray.convertTo(gray, CV_8U);
    }

    Mat globalMask;
    if (config.darkLinesOnLight) {
        threshold(gray, globalMask, config.thresholdValue, 255, THRESH_BINARY_INV);
    } else {
        threshold(gray, globalMask, config.thresholdValue, 255, THRESH_BINARY);
    }

    Mat localMask;
    int blockSize = chooseAdaptiveBlockSize(gray.size());
    int adaptiveBias = max(5, min(15, (255 - config.thresholdValue) / 4 + 3));
    if (config.darkLinesOnLight) {
        adaptiveThreshold(gray, localMask, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY_INV, blockSize, adaptiveBias);
    } else {
        adaptiveThreshold(gray, localMask, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY, blockSize, adaptiveBias);
    }

    Mat mask;
    bitwise_and(globalMask, localMask, mask);
    if (!alpha.empty()) {
        mask.setTo(0, alpha == 0);
    }
    morphologyEx(mask, mask, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, Size(2, 2)));
    if (config.minComponentArea > 1) {
        mask = removeSmallComponentsApi(mask, config.minComponentArea);
    }
    return mask;
}

static Mat makeLinePreviewFromMask(const Mat& mask) {
    if (mask.empty()) return Mat();
    Mat preview;
    bitwise_not(mask, preview);
    return preview;
}

static void clearDirectoryFiles(const string& dirName) {
    string pattern = dirName + "\\*";
    _finddata_t fileInfo{};
    intptr_t handle = _findfirst(pattern.c_str(), &fileInfo);
    if (handle == -1) return;

    while (true) {
        string name = fileInfo.name;
        if (name != "." && name != ".." && (fileInfo.attrib & _A_SUBDIR) == 0) {
            remove((dirName + "\\" + name).c_str());
        }
        if (_findnext(handle, &fileInfo) != 0) break;
    }
    _findclose(handle);
}

static string findDftConfigPath(const string& requested) {
    if (!requested.empty()) return requested;
    if (fileExists("dft_scene_params.txt")) return "dft_scene_params.txt";
    return "";
}

static vector<ApiAttempt> buildApiAttempts(const ApiLineConfig& config) {
    vector<ApiAttempt> attempts;
    auto addAttempt = [&](const string& provider, const string& endpoint, const string& description) {
        attempts.push_back({provider, endpoint, description});
    };

    for (const string& provider : config.providerOrder) {
        if (provider == "responses_image" || provider == "openai_responses_image") {
            addAttempt("responses_image", config.responsesEndpoint, "Responses image path");
        } else if (provider == "openai_compatible" || provider == "openai_compatible_image_edit") {
            addAttempt("openai_compatible", config.endpoint, "OpenAI image edits path");
        } else if (provider == "chat_image" || provider == "openai_chat_image") {
            addAttempt("chat_image", config.chatEndpoint, "Chat completions image path");
        } else if (provider == "image_generation" || provider == "images_generation") {
            addAttempt("openai_compatible", config.imageGenerateEndpoint, "OpenAI image generation path");
        }
    }

    if (attempts.empty()) {
        addAttempt("responses_image", config.responsesEndpoint, "Responses image path");
        addAttempt("openai_compatible", config.endpoint, "OpenAI image edits path");
        addAttempt("chat_image", config.chatEndpoint, "Chat completions image path");
    }
    return attempts;
}

static bool callApiAttempt(const ApiLineConfig& config,
                           const ApiAttempt& attempt,
                           const string& imagePath,
                           vector<unsigned char>& imageBytes,
                           string& error) {
    if (attempt.provider == "responses_image") {
        return callOpenAiResponsesImage(config, imagePath, imageBytes, error);
    }
    if (attempt.provider == "chat_image") {
        return callChatImageStream(config, imagePath, imageBytes, error);
    }
    return callOpenAiCompatibleImageEdit(config, imagePath, imageBytes, error);
}
static string endpointForProvider(const ApiLineConfig& config) {
    if (config.provider == "responses_image" || config.provider == "openai_responses_image") {
        return config.responsesEndpoint;
    }
    if (config.provider == "chat_image" || config.provider == "openai_chat_image") {
        return config.chatEndpoint;
    }
    return config.endpoint;
}

static bool processOneImage(const string& imagePath,
                            const CliOptions& options,
                            const ApiLineConfig& config) {
    string imageName = pathStem(imagePath);
    string outputDir = options.outputRoot + "\\" + imageName;
    string componentDir = outputDir + "\\comp";

    ensureDirectory(options.outputRoot);
    ensureDirectory(outputDir);
    ensureDirectory(componentDir);
    clearDirectoryFiles(componentDir);

    cout << "\n===========================================\n";
    cout << "Input: " << imagePath << "\n";
    cout << "Output: " << outputDir << "\n";
    cout << "Provider: " << config.provider << "\n";
    cout << "Model: " << config.model << "\n";
    string displayEndpoint = endpointForProvider(config);
    if (config.provider == "auto") {
        vector<ApiAttempt> attempts = buildApiAttempts(config);
        if (!attempts.empty()) displayEndpoint = attempts.front().endpoint;
    }
    cout << "Endpoint: " << joinApiUrl(config.baseUrl, displayEndpoint) << "\n";
    cout << "===========================================\n";

    vector<vector<unsigned char>> lineImages;
    if (options.skipApi) {
        string sourceLine = options.localLineImage.empty() ? imagePath : options.localLineImage;
        vector<unsigned char> lineBytes;
        if (!readBinaryFile(sourceLine, lineBytes)) {
            cout << "Error: cannot read local line image: " << sourceLine << "\n";
            return false;
        }
        lineImages.push_back(std::move(lineBytes));
    } else {
        string error;
        if (isPlaceholderModel(config.model) && config.responseModel.empty()) {
            cout << "Error: model is empty or still a placeholder. Fill api_line_config.ini with the model exposed by your relay.\n";
            return false;
        }
        if (config.provider == "auto") {
            cout << "Calling image API with auto fallback...\n";
            bool apiOk = false;
            ostringstream errors;
            vector<ApiAttempt> attempts = buildApiAttempts(config);
            for (size_t i = 0; i < attempts.size(); ++i) {
                const ApiAttempt& attempt = attempts[i];
                cout << "Attempt " << (i + 1) << "/" << attempts.size() << ": "
                     << attempt.description << " ("
                     << joinApiUrl(config.baseUrl, attempt.endpoint) << ")\n";
                string attemptError;
                vector<unsigned char> lineBytes;
                if (callApiAttempt(config, attempt, imagePath, lineBytes, attemptError)) {
                    apiOk = true;
                    lineImages.push_back(std::move(lineBytes));
                    cout << "API attempt succeeded: " << attempt.description << "\n";
                    break;
                }
                if (errors.tellp() > 0) errors << " | ";
                errors << attempt.description << ": " << attemptError;
                cout << "Attempt failed: " << attemptError << "\n";
            }
            if (!apiOk) {
                cout << "API error: all auto provider attempts failed. " << errors.str() << "\n";
                return false;
            }
        } else {
            if (config.provider != "openai_compatible" &&
                config.provider != "openai_compatible_image_edit" &&
                config.provider != "responses_image" &&
                config.provider != "openai_responses_image" &&
                config.provider != "chat_image" &&
                config.provider != "openai_chat_image") {
                cout << "Error: provider '" << config.provider << "' is not implemented yet.\n";
                cout << "Use provider=openai_compatible for OpenAI-style image edit relays, "
                     << "provider=chat_image for streaming chat relays, "
                     << "or provider=responses_image for JSON Responses image generation.\n";
                return false;
            }
            cout << "Calling image API...\n";
            bool apiOk = false;
            if (config.provider == "responses_image" || config.provider == "openai_responses_image") {
                vector<unsigned char> lineBytes;
                apiOk = callOpenAiResponsesImage(config, imagePath, lineBytes, error);
                if (apiOk) lineImages.push_back(std::move(lineBytes));
            } else if (config.provider == "chat_image" || config.provider == "openai_chat_image") {
                vector<unsigned char> lineBytes;
                apiOk = callChatImageStream(config, imagePath, lineBytes, error);
                if (apiOk) lineImages.push_back(std::move(lineBytes));
            } else if (!trimCopy(config.secondaryPrompt).empty()) {
                vector<unsigned char> firstBytes;
                vector<unsigned char> secondBytes;
                string firstError;
                string secondError;
                bool firstOk = callOpenAiCompatibleImageEditWithPrompt(config, imagePath, config.prompt, firstBytes, firstError);
                bool secondOk = callOpenAiCompatibleImageEditWithPrompt(config, imagePath, config.secondaryPrompt, secondBytes, secondError);
                if (firstOk) lineImages.push_back(std::move(firstBytes));
                if (secondOk) lineImages.push_back(std::move(secondBytes));
                apiOk = firstOk && secondOk;
                if (!apiOk) {
                    error = "prompt 1: " + firstError + " | prompt 2: " + secondError;
                }
            } else if (config.imageCount > 1) {
                apiOk = callOpenAiCompatibleImageEditImages(config, imagePath, lineImages, error);
            } else {
                vector<unsigned char> lineBytes;
                apiOk = callOpenAiCompatibleImageEdit(config, imagePath, lineBytes, error);
                if (apiOk) lineImages.push_back(std::move(lineBytes));
            }
            if (!apiOk) {
                cout << "API error: " << error << "\n";
                return false;
            }
        }
    }

    if (lineImages.empty()) {
        cout << "Error: API response did not contain any images.\n";
        return false;
    }

    string primaryChannel = config.primaryChannel.empty() ? "API_Line" : config.primaryChannel;
    vector<string> outputChannels = config.channels;
    if (outputChannels.empty()) outputChannels.push_back(primaryChannel);

    bool anyComponents = false;
    for (size_t imageIndex = 0; imageIndex < lineImages.size(); ++imageIndex) {
        string suffix = imageIndex == 0 ? "" : "_" + to_string(imageIndex + 1);
        string rawPath = outputDir + "\\api_line_raw" + suffix + ".png";
        if (imageIndex == 0) rawPath = outputDir + "\\api_line_raw.png";

        Mat apiLine = imdecode(lineImages[imageIndex], IMREAD_UNCHANGED);
        if (apiLine.empty()) {
            cout << "Error: API response image " << (imageIndex + 1)
                 << " could not be decoded by OpenCV.\n";
            return false;
        }
        if (!options.skipApi && shouldRestoreInputCanvas(config)) {
            Size inputSize = readImageSize(imagePath);
            if (inputSize.width > 0 && inputSize.height > 0 && apiLine.size() != inputSize) {
                apiLine = fitImageOnCanvasPreserveAspect(apiLine, inputSize);
                cout << "  Restored output canvas " << (imageIndex + 1)
                     << " to input size: " << inputSize.width << "x" << inputSize.height << "\n";
            }
        }
        imwrite(rawPath, apiLine);

        Mat mask = buildMaskFromApiLineImage(apiLine, config);
        if (mask.empty() || countNonZero(mask) == 0) {
            cout << "Error: line mask " << (imageIndex + 1) << " is empty after thresholding.\n";
            return false;
        }

        string channelName = imageIndex < outputChannels.size()
                           ? outputChannels[imageIndex]
                           : primaryChannel + "_" + to_string(imageIndex + 1);
        Mat preview = makeLinePreviewFromMask(mask);
        imwrite(outputDir + "\\" + channelName + ".png", preview);
        writeMaskOutputsV2(outputDir, channelName, mask, apiLine.size());
        int guideComponents = writeConnectedComponentSVGsV2(mask, componentDir, channelName, apiLine.size());
        anyComponents = anyComponents || guideComponents > 0;

        cout << "  Raw line image " << (imageIndex + 1) << ": " << rawPath << "\n";
        cout << "  " << channelName << " pixels: " << countNonZero(mask) << "\n";
        cout << "  " << channelName << " components: " << guideComponents << "\n";
    }

    if (!isPotraceAvailableV2()) {
        cout << "Error: potrace is not available on PATH, so SVG/component output is missing.\n";
        return false;
    }
    if (!anyComponents) {
        cout << "Error: no component SVGs were produced.\n";
        return false;
    }

    bool shouldRunDft = options.runDft && config.runDft;
    if (shouldRunDft) {
        string dftConfigPath = findDftConfigPath(options.dftConfigPath);
        map<string, string> dftConfig;
        if (!dftConfigPath.empty()) {
            dftConfig = readDftConfigV2Dft(dftConfigPath);
        }

        vector<string> channels = config.channels;
        if (!options.channelsOverride.empty()) channels = splitCsv(options.channelsOverride);
        if (channels.empty()) channels = {primaryChannel};

        string modeText = options.dftModesOverride.empty() ? config.dftModes : options.dftModesOverride;
        vector<string> modes = dftModesFromText(modeText);

        cout << "Running DFT precompute";
        if (!dftConfigPath.empty()) cout << " with config " << dftConfigPath;
        cout << "...\n";
        if (!precomputeDftForImageV2(options.outputRoot, imageName, channels, modes, dftConfig)) {
            cout << "Error: DFT precompute failed.\n";
            return false;
        }
    }

    return true;
}

static void printUsage() {
    cout << "Usage: Fourier-api-approach.exe [options] <image ...>\n"
         << "Options:\n"
         << "  --config <file>       API config, default api_line_config.ini\n"
         << "  --output <dir>        Output root, default results_api\n"
         << "  --dft-config <file>   DFT params file; defaults to local dft_scene_params.txt\n"
         << "  --modes <value>       sequential, simultaneous, full_svg, both, or all\n"
         << "  --channels <csv>      DFT channels, default from config\n"
         << "  --no-dft             Skip DFT precompute\n"
         << "  --precompute-only    Only regenerate DFT data from an existing output folder\n"
         << "  --local-line <file>   Skip API and run SVG/component/DFT from an existing line image\n"
         << "  --help, -h            Show this help\n";
}

static CliOptions parseCli(int argc, char** argv) {
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if ((arg == "--help" || arg == "-h")) {
            printUsage();
            exit(0);
        } else if (arg == "--config" && i + 1 < argc) {
            options.configPath = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            options.outputRoot = argv[++i];
        } else if (arg == "--dft-config" && i + 1 < argc) {
            options.dftConfigPath = argv[++i];
        } else if ((arg == "--mode" || arg == "--modes") && i + 1 < argc) {
            options.dftModesOverride = argv[++i];
        } else if (arg == "--channels" && i + 1 < argc) {
            options.channelsOverride = argv[++i];
        } else if (arg == "--no-dft") {
            options.runDft = false;
        } else if (arg == "--precompute-only") {
            options.precomputeOnly = true;
        } else if (arg == "--local-line" && i + 1 < argc) {
            options.localLineImage = argv[++i];
            options.skipApi = true;
        } else {
            options.imagePaths.push_back(arg);
        }
    }
    return options;
}

int main(int argc, char** argv) {
    cout << "===========================================\n";
    cout << "  LLM Line Art to Fourier Components\n";
    cout << "===========================================\n\n";

    CliOptions options = parseCli(argc, argv);
    if (options.imagePaths.empty()) {
        cout << "Error: no input image provided.\n\n";
        printUsage();
        return 1;
    }

    ApiLineConfig config = loadApiLineConfig(options.configPath);
    if (!fileExists(options.configPath) && !options.skipApi && !options.precomputeOnly) {
        cout << "Error: config file not found: " << options.configPath << "\n";
        cout << "Copy api_line_config.example.ini to api_line_config.ini and fill base_url/api_key/model.\n";
        return 1;
    }

    if (!options.channelsOverride.empty()) {
        config.channels = splitCsv(options.channelsOverride);
    }

    cout << "Config: " << options.configPath << "\n";
    cout << "Output root: " << options.outputRoot << "\n";
    cout << "DFT: " << ((options.runDft && config.runDft) ? "enabled" : "disabled") << "\n";

    if (options.precomputeOnly) {
        string dftConfigPath = findDftConfigPath(options.dftConfigPath);
        map<string, string> dftConfig;
        if (!dftConfigPath.empty()) {
            dftConfig = readDftConfigV2Dft(dftConfigPath);
        }

        vector<string> channels = config.channels;
        if (!options.channelsOverride.empty()) channels = splitCsv(options.channelsOverride);
        if (channels.empty()) channels = {config.primaryChannel.empty() ? "API_Line" : config.primaryChannel};

        string modeText = options.dftModesOverride.empty() ? config.dftModes : options.dftModesOverride;
        vector<string> modes = dftModesFromText(modeText);

        int successCount = 0;
        for (const string& imagePath : options.imagePaths) {
            string imageName = pathStem(imagePath);
            if (precomputeDftForImageV2(options.outputRoot, imageName, channels, modes, dftConfig)) {
                ++successCount;
            }
        }
        cout << "\nDFT precomputed " << successCount << " of " << options.imagePaths.size() << " images\n";
        return successCount == static_cast<int>(options.imagePaths.size()) ? 0 : 1;
    }

    int successCount = 0;
    for (const string& imagePath : options.imagePaths) {
        if (processOneImage(imagePath, options, config)) {
            ++successCount;
        }
    }

    cout << "\n===========================================\n";
    cout << "Processed " << successCount << " of " << options.imagePaths.size() << " images\n";
    cout << "Output folders are under " << options.outputRoot << "\\<image-name>\\\n";
    cout << "===========================================\n";
    return successCount == static_cast<int>(options.imagePaths.size()) ? 0 : 1;
}





