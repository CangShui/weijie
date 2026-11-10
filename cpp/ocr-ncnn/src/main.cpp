#include "ocr_engine.h"

#include <gpu.h>

#include <fcntl.h>
#include <io.h>

#include <iostream>
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

std::unique_ptr<OCR::OCREngine> make_engine(const char* config) {
    auto engine = std::make_unique<OCR::OCREngine>();
    if (!engine->Initialize(config)) return {};
    return engine;
}

}  // namespace

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::cout << "READY\n" << std::flush;

    std::unique_ptr<OCR::OCREngine> gpu_engine;
    std::unique_ptr<OCR::OCREngine> cpu_engine;
    bool gpu_tried = false;

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

        bool want_gpu = device_line == "GPU" || device_line == "gpu";
        if (want_gpu && !gpu_tried) {
            gpu_tried = true;
            if (ncnn::get_gpu_count() > 0) {
                gpu_engine = make_engine("config-gpu.json");
            }
        }

        OCR::OCREngine* engine = nullptr;
        if (want_gpu) engine = gpu_engine.get();
        if (!engine) {
            if (!cpu_engine) cpu_engine = make_engine("config-cpu.json");
            engine = cpu_engine.get();
        }
        if (!engine) {
            respond("ERR", "failed to initialize PP-OCRv6 ncnn engine");
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
