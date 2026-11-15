#include "ocr_engine.h"

#include <gpu.h>

#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace {

std::string trim_copy(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) ++start;
    return value.substr(start);
}

void respond(const char* status, const std::string& payload) {
    std::cout << status << ' ' << payload.size() << '\n';
    std::cout.write(payload.data(), (std::streamsize)payload.size());
    std::cout << '\n';
    std::cout.flush();
}

std::unique_ptr<OCR::OCREngine> make_engine(const char* config, const std::string& profile) {
    auto engine = std::make_unique<OCR::OCREngine>();
    if (!engine->Initialize(config, profile)) return {};
    return engine;
}

}  // namespace

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::cout << "READY\n" << std::flush;

    // One engine per (device, model profile) pair. Profiles select the tiny /
    // small / medium PP-OCRv6 weights declared in config-*.json.
    std::map<std::string, std::unique_ptr<OCR::OCREngine>> engines;
    std::map<std::string, bool> failed;
    bool gpu_probed = false;
    bool gpu_available = false;

    std::string device_line;
    while (std::getline(std::cin, device_line)) {
        device_line = trim_copy(device_line);
        if (device_line.empty()) continue;

        std::string image_path;
        if (!std::getline(std::cin, image_path)) break;
        image_path = trim_copy(image_path);
        if (image_path.empty()) {
            respond("ERR", "empty image path");
            continue;
        }

        std::string device = device_line;
        std::string profile;
        const size_t space = device_line.find_first_of(" \t");
        if (space != std::string::npos) {
            profile = trim_copy(device_line.substr(space + 1));
            device = device_line.substr(0, space);
        }
        bool want_gpu = device == "GPU" || device == "gpu";
        if (want_gpu && !gpu_probed) {
            gpu_probed = true;
            gpu_available = ncnn::get_gpu_count() > 0;
        }
        if (want_gpu && !gpu_available) want_gpu = false;

        auto obtain = [&](bool gpu) -> OCR::OCREngine* {
            const std::string key = std::string(gpu ? "gpu:" : "cpu:") + profile;
            auto it = engines.find(key);
            if (it != engines.end()) return it->second.get();
            if (failed[key]) return nullptr;
            auto created = make_engine(gpu ? "config-gpu.json" : "config-cpu.json", profile);
            if (!created) {
                failed[key] = true;
                return nullptr;
            }
            OCR::OCREngine* raw = created.get();
            engines[key] = std::move(created);
            return raw;
        };

        OCR::OCREngine* engine = obtain(want_gpu);
        if (!engine && want_gpu) engine = obtain(false);
        if (!engine) {
            respond("ERR", profile.empty()
                ? "failed to initialize PP-OCRv6 ncnn engine"
                : "failed to initialize PP-OCRv6 ncnn engine for profile " + profile);
            continue;
        }

        try {
            cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR_BGR);
            if (image.empty()) {
                respond("ERR", "failed to read image");
                continue;
            }
            auto results = engine->Run(image);
            std::string text;
            for (const auto& result : results) {
                if (!text.empty()) text.push_back('\n');
                text += result.line.text;
            }
            if (text.empty()) {
                respond("ERR", "no text recognized");
                continue;
            }
            respond("OK", text);
        } catch (const std::exception& e) {
            respond("ERR", std::string("OCR exception: ") + e.what());
        } catch (...) {
            respond("ERR", "OCR unknown exception");
        }
    }
    return 0;
}
