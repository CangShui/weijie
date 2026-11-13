#include "settings.h"
#include "ocr.h"

const HotkeyPreset HOTKEY_PRESETS[9] = {
    {L"Alt+A",        MOD_ALT,                  0x41},
    {L"Ctrl+Alt+A",   MOD_CONTROL | MOD_ALT,    0x41},
    {L"Ctrl+Shift+A", MOD_CONTROL | MOD_SHIFT,  0x41},
    {L"Alt+Z",        MOD_ALT,                  0x5A},
    {L"Ctrl+Alt+Z",   MOD_CONTROL | MOD_ALT,    0x5A},
    {L"Alt+F9",       MOD_ALT,                  0x78},
    {L"Ctrl+Shift+S", MOD_CONTROL | MOD_SHIFT,  0x53},
    {L"Ctrl+Alt+S",   MOD_CONTROL | MOD_ALT,    0x53},
    {L"PrintScreen",  0,                        VK_SNAPSHOT},
};
const int N_HOTKEY_PRESETS = 9;

bool gpu_ocr_supported() {
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    bool intel = false;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        std::wstring id = dd.DeviceID;
        std::wstring name = dd.DeviceString;
        for (wchar_t& c : id) c = (wchar_t)towupper(c);
        for (wchar_t& c : name) c = (wchar_t)towupper(c);
        if (id.find(L"VEN_8086") != std::wstring::npos ||
            name.find(L"INTEL") != std::wstring::npos) intel = true;
        // 当前 GPU 后端不支持 AMD / NVIDIA 独显；混合显卡机器也保守回退 CPU。
        if (id.find(L"VEN_10DE") != std::wstring::npos ||
            id.find(L"VEN_1002") != std::wstring::npos ||
            name.find(L"NVIDIA") != std::wstring::npos ||
            name.find(L"AMD") != std::wstring::npos ||
            name.find(L"RADEON") != std::wstring::npos) return false;
        dd = {};
        dd.cb = sizeof(dd);
    }
    return intel;
}

std::wstring settings_path() {
    return appdata_dir() + L"\\settings.json";
}

bool resolve_hotkey(const std::wstring& name, UINT& mods, UINT& vk) {
    for (int i = 0; i < N_HOTKEY_PRESETS; ++i) {
        if (name == HOTKEY_PRESETS[i].name) {
            mods = HOTKEY_PRESETS[i].mods;
            vk = HOTKEY_PRESETS[i].vk;
            return true;
        }
    }
    mods = 0;
    vk = 0;
    std::wstring value;
    for (wchar_t c : name) if (!iswspace(c)) value += towupper(c);
    size_t pos = 0;
    while (true) {
        size_t end = value.find(L'+', pos);
        std::wstring part = value.substr(pos, end == std::wstring::npos ? end : end - pos);
        if (end == std::wstring::npos) {
            if (part.size() == 1 && ((part[0] >= L'A' && part[0] <= L'Z') ||
                                    (part[0] >= L'0' && part[0] <= L'9'))) vk = part[0];
            else if (part == L"PRINTSCREEN") vk = VK_SNAPSHOT;
            else if (part.size() >= 2 && part[0] == L'F') {
                int n = 0;
                for (size_t i = 1; i < part.size(); ++i) {
                    if (part[i] < L'0' || part[i] > L'9' || n > 24) return false;
                    n = n * 10 + part[i] - L'0';
                }
                if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
            }
            break;
        }
        UINT bit = part == L"CTRL" ? MOD_CONTROL : part == L"ALT" ? MOD_ALT :
                   part == L"SHIFT" ? MOD_SHIFT : 0;
        if (!bit || (mods & bit)) return false;
        mods |= bit;
        pos = end + 1;
    }
    return vk && ((mods & (MOD_CONTROL | MOD_ALT)) ||
                  (vk >= VK_F1 && vk <= VK_F24) || vk == VK_SNAPSHOT);
}

// ------------------------------------------------------------ 极简 JSON 读取
static std::string read_all_utf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::string out;
    if (sz.QuadPart > 0 && sz.QuadPart < (1 << 20)) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
        out.resize(got);
    }
    CloseHandle(h);
    return out;
}

static std::string get_json_str(const std::string& js, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return std::string();
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return std::string();
    p = js.find('"', p);
    if (p == std::string::npos) return std::string();
    std::string out;
    for (size_t i = p + 1; i < js.size(); ++i) {
        char c = js[i];
        if (c == '\\' && i + 1 < js.size()) {
            char n = js[++i];
            switch (n) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 < js.size()) {
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = js[++i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    }
                    wchar_t wbuf[2] = {(wchar_t)cp, 0};
                    out += w_to_u8(wbuf);
                }
                break;
            }
            default: out.push_back(n);
            }
        } else if (c == '"') {
            break;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static int get_json_int(const std::string& js, const char* key, int fallback) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return fallback;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return fallback;
    ++p;
    while (p < js.size() && (js[p] == ' ' || js[p] == '\t')) ++p;
    bool neg = false;
    if (p < js.size() && js[p] == '-') { neg = true; ++p; }
    int v = 0;
    bool ok = false;
    while (p < js.size() && js[p] >= '0' && js[p] <= '9') {
        ok = true;
        if (v > 100000) return fallback;
        v = v * 10 + (js[p] - '0');
        ++p;
    }
    if (!ok) return fallback;
    return neg ? -v : v;
}

static bool get_json_bool(const std::string& js, const char* key, bool fallback) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return fallback;
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return fallback;
    ++p;
    while (p < js.size() && (js[p] == ' ' || js[p] == '\t')) ++p;
    if (js.compare(p, 4, "true") == 0) return true;
    if (js.compare(p, 5, "false") == 0) return false;
    return fallback;
}

Settings load_settings() {
    Settings s;
    std::string js = read_all_utf8(settings_path());
    if (js.empty()) return s;
    std::string v;
    if (!(v = get_json_str(js, "hotkey")).empty())     s.hotkey = u8_to_w(v);
    s.hotkey_enabled = get_json_bool(js, "hotkey_enabled", true);
    if (!(v = get_json_str(js, "save_dir")).empty())   s.save_dir = u8_to_w(v);
    if (!(v = get_json_str(js, "image_format")).empty()) s.image_format = u8_to_w(v);
    s.jpeg_quality = get_json_int(js, "jpeg_quality", s.jpeg_quality);
    if (s.jpeg_quality < 1) s.jpeg_quality = 1;
    if (s.jpeg_quality > 100) s.jpeg_quality = 100;
    s.auto_copy = get_json_bool(js, "auto_copy", s.auto_copy);
    s.include_cursor = get_json_bool(js, "include_cursor", s.include_cursor);
    s.stroke_width = std::clamp(get_json_int(js, "stroke_width", 3), 1, 30);
    s.recent_colors=u8_to_w(get_json_str(js,"recent_colors"));
    if(!(v=get_json_str(js,"ocr_backend")).empty())s.ocr_backend=u8_to_w(v);
    if(!(v=get_json_str(js,"ocr_engine")).empty())s.ocr_engine=u8_to_w(v);
    if(s.ocr_engine.empty()||s.ocr_engine==L"fast")s.ocr_engine=L"ppocr";
    if(!(v=get_json_str(js,"ocr_benchmark")).empty())s.ocr_benchmark=u8_to_w(v);
    s.text_size = std::clamp(get_json_int(js, "text_size", 18), 10, 72);
    s.ocr_enabled = get_json_bool(js, "ocr_enabled", s.ocr_enabled);
    int idle=get_json_int(js,"ocr_idle_minutes",0);
    if(std::find(std::begin(OCR_IDLE_TIMEOUT_MINUTES),std::end(OCR_IDLE_TIMEOUT_MINUTES),idle)
       !=std::end(OCR_IDLE_TIMEOUT_MINUTES))s.ocr_idle_minutes=idle;
    else{
        LogTrace trace;
        logf(L"读取设置：OCR 空闲回收分钟=%d 不在0/1/10/60/720/1440范围，使用off",idle);
    }
    s.translate_enabled = get_json_bool(js, "translate_enabled", s.translate_enabled);
    if (!(v = get_json_str(js, "ocr_model_dir")).empty()) s.ocr_model_dir = u8_to_w(v);
    if (!(v = get_json_str(js, "llm_base")).empty())   s.llm_base = u8_to_w(v);
    if (!(v = get_json_str(js, "llm_model")).empty())  s.llm_model = u8_to_w(v);
    v = get_json_str(js, "llm_key");
    s.llm_key = u8_to_w(v);   // 允许为空
    return s;
}

bool save_settings(const Settings& s) {
    LogTrace trace;
    logf(L"设置空闲回收校验：分钟=%d 允许=0/1/10/60/720/1440",s.ocr_idle_minutes);
    if(std::find(std::begin(OCR_IDLE_TIMEOUT_MINUTES),std::end(OCR_IDLE_TIMEOUT_MINUTES),s.ocr_idle_minutes)
       ==std::end(OCR_IDLE_TIMEOUT_MINUTES)){
        logf(L"设置写入被拒绝：OCR 空闲回收时间不在下拉选项范围，未写入或应用设置");
        return false;
    }
    bool previousOcrEnabled=load_settings().ocr_enabled;
    logf(L"设置写入开始：文件=%s 保存目录=%s OCR启用=%d 引擎=%s",
         settings_path().c_str(),s.save_dir.c_str(),(int)s.ocr_enabled,s.ocr_engine.c_str());
    std::string js = "{\n";
    js += "  \"recent_colors\": \"" + json_escape(w_to_u8(s.recent_colors)) + "\",\n";
    js += "  \"ocr_backend\": \"" + json_escape(w_to_u8(s.ocr_backend)) + "\",\n";
    js += "  \"ocr_engine\": \"" + json_escape(w_to_u8(s.ocr_engine)) + "\",\n";
    js += "  \"ocr_benchmark\": \"" + json_escape(w_to_u8(s.ocr_benchmark)) + "\",\n";
    js += "  \"hotkey\": \"" + json_escape(w_to_u8(s.hotkey)) + "\",\n";
    js += "  \"hotkey_enabled\": " + std::string(s.hotkey_enabled ? "true" : "false") + ",\n";
    js += "  \"save_dir\": \"" + json_escape(w_to_u8(s.save_dir)) + "\",\n";
    js += "  \"image_format\": \"" + json_escape(w_to_u8(s.image_format)) + "\",\n";
    js += "  \"jpeg_quality\": " + std::to_string(s.jpeg_quality) + ",\n";
    js += "  \"auto_copy\": " + std::string(s.auto_copy ? "true" : "false") + ",\n";
    js += "  \"include_cursor\": " + std::string(s.include_cursor ? "true" : "false") + ",\n";
    js += "  \"stroke_width\": " + std::to_string(s.stroke_width) + ",\n";
    js += "  \"text_size\": " + std::to_string(s.text_size) + ",\n";
    js += "  \"ocr_enabled\": " + std::string(s.ocr_enabled ? "true" : "false") + ",\n";
    js += "  \"ocr_idle_minutes\": " + std::to_string(s.ocr_idle_minutes) + ",\n";
    js += "  \"translate_enabled\": " + std::string(s.translate_enabled ? "true" : "false") + ",\n";
    js += "  \"ocr_model_dir\": \"" + json_escape(w_to_u8(s.ocr_model_dir)) + "\",\n";
    js += "  \"llm_base\": \"" + json_escape(w_to_u8(s.llm_base)) + "\",\n";
    js += "  \"llm_key\": \"" + json_escape(w_to_u8(s.llm_key)) + "\",\n";
    js += "  \"llm_model\": \"" + json_escape(w_to_u8(s.llm_model)) + "\"\n";
    js += "}\n";

    std::wstring path = settings_path();
    std::wstring temp = path + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        logf(L"设置写入失败：无法创建临时文件 error=%lu，原设置未改动",GetLastError());
        return false;
    }
    DWORD wrote = 0;
    bool ok = WriteFile(h, js.data(), (DWORD)js.size(), &wrote, nullptr) &&
              wrote == js.size() && FlushFileBuffers(h);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        if (!error) error=GetLastError();
        DeleteFileW(temp.c_str());
        logf(L"设置写入返回：失败 error=%lu 写入字节=%lu 预期字节=%zu，未应用设置",
             error,wrote,js.size());
    } else {
        logf(L"设置写入返回：成功 字节=%lu 文件=%s",wrote,path.c_str());
        if(!s.ocr_enabled||previousOcrEnabled!=s.ocr_enabled)ocr_set_enabled(s.ocr_enabled);
        ocr_set_idle_timeout(s.ocr_idle_minutes);
    }
    return ok;
}
