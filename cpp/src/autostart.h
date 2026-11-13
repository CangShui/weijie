#pragma once
#include "common.h"
#include <taskschd.h>

// 当前用户登录后，在交互桌面上以最高权限运行微截。
bool autostart_enabled();
bool set_autostart(bool enabled, std::wstring& error);
bool migrate_legacy_autostart(std::wstring& error);

HRESULT create_autostart_definition(ITaskService* service, const std::wstring& userSid,
                                    const std::wstring& executable, ITaskDefinition** definition);
