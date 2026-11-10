#pragma once
#include "common.h"
#include "settings.h"

struct TranslateResult {
    std::wstring text;
    std::wstring src;
    std::wstring dst;
    std::wstring err;
    bool ok() const { return err.empty(); }
};

TranslateResult translate_text(const std::wstring& text, const Settings& cfg);
