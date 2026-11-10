#include "translate.h"
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// ------------------------------------------------------------------ HTTP
struct UrlParts {
    bool https = true;
    std::wstring host;
    std::wstring path;   // 含 query
    INTERNET_PORT port = 443;
};

static bool parse_url(const std::wstring& url, UrlParts& out) {
    std::wstring s = url;
    if (s.rfind(L"https://", 0) == 0) { out.https = true; s = s.substr(8); out.port = 443; }
    else if (s.rfind(L"http://", 0) == 0) { out.https = false; s = s.substr(7); out.port = 80; }
    else return false;

    size_t slash = s.find(L'/');
    std::wstring hostport = (slash == std::wstring::npos) ? s : s.substr(0, slash);
    out.path = (slash == std::wstring::npos) ? L"/" : s.substr(slash);

    size_t colon = hostport.find(L':');
    if (colon != std::wstring::npos) {
        out.host = hostport.substr(0, colon);
        out.port = (INTERNET_PORT)_wtoi(hostport.substr(colon + 1).c_str());
    } else {
        out.host = hostport;
    }
    return !out.host.empty();
}

static bool http_do(const std::wstring& url, const std::wstring& verb,
                    const std::string& body, const std::wstring& content_type,
                    std::string& out, std::wstring& err) {
    out.clear();
    UrlParts u;
    if (!parse_url(url, u)) { err = L"地址无效"; return false; }

    HINTERNET ses = WinHttpOpen(L"weijie/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        ses = WinHttpOpen(L"weijie/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!ses) { err = L"网络初始化失败"; return false; }

    int timeout = 15000;
    WinHttpSetTimeouts(ses, timeout, timeout, timeout, timeout);

    HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0);
    if (!con) { WinHttpCloseHandle(ses); err = L"无法连接服务器"; return false; }

    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, verb.c_str(), u.path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        err = L"创建请求失败";
        return false;
    }

    std::wstring headers = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                           L"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36\r\n";
    if (!content_type.empty()) headers += L"Content-Type: " + content_type + L"\r\n";

    BOOL ok = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L,
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);

    DWORD status = 0;
    DWORD len = sizeof(status);
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    if (ok) {
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) break;
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got)) break;
            out.append(chunk.data(), got);
        } while (avail > 0);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);

    if (!ok) { err = L"网络请求失败"; return false; }
    if (status == 429) { err = L"翻译接口请求过于频繁，请稍后再试"; return false; }
    if (status >= 400) {
        wchar_t buf[64];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"HTTP %u", status);
        err = buf;
        return false;
    }
    if (out.empty()) { err = L"服务器返回为空"; return false; }
    return true;
}

// ------------------------------------------------------------------ JSON 取值
static void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static unsigned parse_hex4(const std::string& s, size_t i) {
    unsigned v = 0;
    for (int k = 0; k < 4 && i + k < s.size(); ++k) {
        char c = s[i + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    }
    return v;
}

// 找到 "key" 之后第一个字符串值
static std::string json_str_after(const std::string& js, size_t from, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat, from);
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
            case 'b': break;
            case 'f': break;
            case 'u': {
                unsigned cp = parse_hex4(js, i + 1);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < js.size() &&
                    js[i + 1] == '\\' && js[i + 2] == 'u') {
                    unsigned lo = parse_hex4(js, i + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                append_utf8(out, cp);
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

static std::string html_unescape(std::string s) {
    struct { const char* ent; const char* rep; } tbl[] = {
        {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "},
    };
    for (auto& e : tbl) {
        size_t p = 0;
        while ((p = s.find(e.ent, p)) != std::string::npos) {
            s.replace(p, strlen(e.ent), e.rep);
            p += strlen(e.rep);
        }
    }
    return s;
}

// ------------------------------------------------------------------ 语言判定
static void detect(const std::wstring& text, std::wstring& src, std::wstring& dst) {
    int cjk = 0, latin = 0;
    for (wchar_t c : text) {
        unsigned u = (unsigned)c;
        if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3400 && u <= 0x4DBF)) ++cjk;
        else if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) ++latin;
    }
    if (cjk >= latin) { src = L"zh-CN"; dst = L"en"; }
    else { src = L"en"; dst = L"zh-CN"; }
}

// 按行/句切块
static std::vector<std::wstring> chunks(const std::wstring& text, size_t limit = 450) {
    std::vector<std::wstring> parts;
    std::wstring buf;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        size_t end = (nl == std::wstring::npos) ? text.size() : nl + 1;
        std::wstring piece = text.substr(pos, end - pos);
        pos = end;

        while (piece.size() > limit) {
            size_t cut = piece.rfind(L'\u3002', limit);
            if (cut == std::wstring::npos || cut < limit / 3) cut = piece.rfind(L". ", limit);
            if (cut == std::wstring::npos || cut < limit / 3) cut = limit - 1;
            parts.push_back(piece.substr(0, cut + 1));
            piece = piece.substr(cut + 1);
        }
        if (buf.size() + piece.size() > limit) {
            if (!buf.empty()) parts.push_back(buf);
            buf = piece;
        } else {
            buf += piece;
        }
    }
    if (!buf.empty()) parts.push_back(buf);

    std::vector<std::wstring> out;
    for (auto& p : parts) {
        bool has = false;
        for (wchar_t c : p) if (!iswspace(c)) { has = true; break; }
        if (has) out.push_back(p);
    }
    if (out.empty()) out.push_back(text);
    return out;
}

// ------------------------------------------------------------------ 引擎
static bool translate_mymemory(const std::wstring& text, const std::wstring& src,
                               const std::wstring& dst, std::wstring& out, std::wstring& err) {
    std::wstring joined;
    for (auto& ch : chunks(text)) {
        std::string q = url_encode(w_to_u8(ch));
        std::wstring url = L"https://api.mymemory.translated.net/get?q=" + u8_to_w(q) +
                           L"&langpair=" + src + L"|" + dst;
        std::string body;
        std::wstring e2;
        if (!http_do(url, L"GET", std::string(), L"", body, e2)) {
            err = e2;
            return false;
        }
        std::string t = json_str_after(body, 0, "translatedText");
        if (t.empty()) {
            std::string detail = json_str_after(body, 0, "responseDetails");
            err = detail.empty() ? L"翻译服务返回为空" : u8_to_w(detail);
            return false;
        }
        joined += u8_to_w(html_unescape(t));
    }
    out = joined;
    return true;
}

static bool translate_llm(const std::wstring& text, const std::wstring& src,
                          const std::wstring& dst, const Settings& cfg,
                          std::wstring& out, std::wstring& err) {
    std::wstring base = cfg.llm_base.empty() ? L"https://api.deepseek.com/v1" : cfg.llm_base;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    std::wstring model = cfg.llm_model.empty() ? L"deepseek-chat" : cfg.llm_model;

    std::string sys = "你是一个翻译引擎。只输出译文，不要解释、不要引号、不要加任何前后缀。";
    sys += (dst == L"en") ? "把用户给的文本翻译成英文，" : "把用户给的文本翻译成简体中文，";
    sys += "保持原有的换行与分段。";

    std::string body = "{\"model\":\"" + json_escape(w_to_u8(model)) + "\",\"temperature\":0.2,";
    body += "\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(sys) + "\"},";
    body += "{\"role\":\"user\",\"content\":\"" + json_escape(w_to_u8(text)) + "\"}]}";

    std::string resp;
    std::wstring e2;
    std::wstring url = base + L"/chat/completions";
    if (!http_do(url, L"POST", body, L"application/json", resp, e2)) {
        err = e2;
        return false;
    }
    size_t choices = resp.find("\"choices\"");
    std::string content = json_str_after(resp, choices == std::string::npos ? 0 : choices, "content");
    if (content.empty()) {
        err = L"大模型返回异常";
        return false;
    }
    // 去掉首尾空白
    std::wstring w = u8_to_w(content);
    size_t b = w.find_first_not_of(L" \r\n\t");
    size_t e = w.find_last_not_of(L" \r\n\t");
    out = (b == std::wstring::npos) ? L"" : w.substr(b, e - b + 1);
    return true;
}

TranslateResult translate_text(const std::wstring& text, const Settings& cfg) {
    TranslateResult r;
    std::wstring src = L"zh-CN", dst = L"en";
    detect(text, src, dst);
    r.src = src;
    r.dst = dst;

    std::wstring t = text;
    size_t b = t.find_first_not_of(L" \r\n\t");
    size_t e = t.find_last_not_of(L" \r\n\t");
    t = (b == std::wstring::npos) ? L"" : t.substr(b, e - b + 1);
    if (t.empty()) {
        r.err = L"没有可翻译的文字";
        return r;
    }

    std::wstring out, err;
    bool ok = cfg.llm_key.empty() ? translate_mymemory(t, src, dst, out, err)
                                  : translate_llm(t, src, dst, cfg, out, err);
    if (!ok) {
        r.err = err;
        return r;
    }
    r.text = out;
    return r;
}
