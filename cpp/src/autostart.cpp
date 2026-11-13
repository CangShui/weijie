#include "autostart.h"
#include <comdef.h>
#include <sddl.h>
#include <filesystem>
#include <wrl/client.h>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;
static const wchar_t* RUN_KEY=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE=L"微截";

static HRESULT user_sid(std::wstring& sid){
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))
        return HRESULT_FROM_WIN32(GetLastError());
    DWORD size=0;
    GetTokenInformation(token,TokenUser,nullptr,0,&size);
    std::vector<BYTE> data(size);
    BOOL ok=GetTokenInformation(token,TokenUser,data.data(),size,&size);
    DWORD error=ok?ERROR_SUCCESS:GetLastError();
    CloseHandle(token);
    if(!ok)return HRESULT_FROM_WIN32(error);
    LPWSTR text=nullptr;
    if(!ConvertSidToStringSidW(((TOKEN_USER*)data.data())->User.Sid,&text))
        return HRESULT_FROM_WIN32(GetLastError());
    sid=text;LocalFree(text);
    return S_OK;
}

static std::wstring task_name(const std::wstring& sid){return L"weijie-logon-"+sid;}

static HRESULT scheduler(ComPtr<ITaskService>& service,ComPtr<ITaskFolder>& folder){
    HRESULT hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service));
    if(FAILED(hr))return hr;
    hr=service->Connect(_variant_t(),_variant_t(),_variant_t(),_variant_t());
    if(FAILED(hr))return hr;
    return service->GetFolder(_bstr_t(L"\\"),&folder);
}

#define TASK_CHECK(call) do { HRESULT result=(call);if(FAILED(result)){ \
    logf(L"管理员登录任务定义创建失败：HRESULT=0x%08X",(unsigned)result);return result;} } while(0)

HRESULT create_autostart_definition(ITaskService* service,const std::wstring& userSid,
                                    const std::wstring& executable,ITaskDefinition** definition){
    if(!service||!definition)return E_POINTER;
    *definition=nullptr;
    if(userSid.empty()||executable.empty())return E_INVALIDARG;
    LogTrace trace;
    logf(L"管理员登录任务定义开始：用户=%s 程序=%s",userSid.c_str(),executable.c_str());
    ComPtr<ITaskDefinition> task;
    TASK_CHECK(service->NewTask(0,&task));
    ComPtr<IRegistrationInfo> info;
    TASK_CHECK(task->get_RegistrationInfo(&info));
    TASK_CHECK(info->put_Author(_bstr_t(L"weijie")));
    TASK_CHECK(info->put_Description(_bstr_t(L"用户登录后以管理员权限启动微截，用于游戏截图。")));
    ComPtr<IPrincipal> principal;
    TASK_CHECK(task->get_Principal(&principal));
    TASK_CHECK(principal->put_UserId(_bstr_t(userSid.c_str())));
    TASK_CHECK(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
    TASK_CHECK(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
    ComPtr<ITaskSettings> settings;
    TASK_CHECK(task->get_Settings(&settings));
    TASK_CHECK(settings->put_Enabled(VARIANT_TRUE));
    TASK_CHECK(settings->put_StartWhenAvailable(VARIANT_TRUE));
    TASK_CHECK(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
    TASK_CHECK(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
    TASK_CHECK(settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S")));
    TASK_CHECK(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
    ComPtr<ITriggerCollection> triggers;
    TASK_CHECK(task->get_Triggers(&triggers));
    ComPtr<ITrigger> trigger;
    TASK_CHECK(triggers->Create(TASK_TRIGGER_LOGON,&trigger));
    ComPtr<ILogonTrigger> logon;
    TASK_CHECK(trigger.As(&logon));
    TASK_CHECK(logon->put_UserId(_bstr_t(userSid.c_str())));
    TASK_CHECK(logon->put_Enabled(VARIANT_TRUE));
    ComPtr<IActionCollection> actions;
    TASK_CHECK(task->get_Actions(&actions));
    ComPtr<IAction> action;
    TASK_CHECK(actions->Create(TASK_ACTION_EXEC,&action));
    ComPtr<IExecAction> exec;
    TASK_CHECK(action.As(&exec));
    TASK_CHECK(exec->put_Path(_bstr_t(executable.c_str())));
    TASK_CHECK(exec->put_Arguments(_bstr_t(L"--autostart")));
    TASK_CHECK(exec->put_WorkingDirectory(_bstr_t(std::filesystem::path(executable).parent_path().c_str())));
    logf(L"管理员登录任务定义返回：成功，当前用户登录触发、交互桌面、最高权限、电池供电可运行、无运行时限");
    *definition=task.Detach();
    return S_OK;
}
#undef TASK_CHECK

static bool failed(const wchar_t* step,HRESULT hr,std::wstring& error){
    wchar_t code[32];swprintf_s(code,L"0x%08X",(unsigned)hr);
    error=std::wstring(step)+L"失败（"+code+L"）。请确认微截以管理员身份运行，且 Windows 任务计划程序服务可用。";
    logf(L"管理员开机启动返回：失败 阶段=%s HRESULT=%s，未报告设置成功",step,code);
    return false;
}

static HRESULT remove_legacy_run(){
    HKEY key=nullptr;
    LSTATUS status=RegOpenKeyExW(HKEY_CURRENT_USER,RUN_KEY,0,KEY_SET_VALUE,&key);
    if(status==ERROR_FILE_NOT_FOUND)return S_OK;
    if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
    status=RegDeleteValueW(key,RUN_VALUE);
    RegCloseKey(key);
    if(status==ERROR_FILE_NOT_FOUND)return S_OK;
    return HRESULT_FROM_WIN32(status);
}

bool autostart_enabled(){
    std::wstring sid;
    HRESULT hr=user_sid(sid);
    if(FAILED(hr)){logf(L"查询管理员开机启动失败：无法读取当前用户 HRESULT=0x%08X",(unsigned)hr);return false;}
    ComPtr<ITaskService> service;ComPtr<ITaskFolder> folder;
    hr=scheduler(service,folder);
    if(FAILED(hr)){logf(L"查询管理员开机启动失败：任务计划服务 HRESULT=0x%08X",(unsigned)hr);return false;}
    ComPtr<IRegisteredTask> registered;
    hr=folder->GetTask(_bstr_t(task_name(sid).c_str()),&registered);
    if(FAILED(hr))return false;
    VARIANT_BOOL enabled=VARIANT_FALSE;
    if(FAILED(registered->get_Enabled(&enabled))||enabled!=VARIANT_TRUE)return false;
    ComPtr<ITaskDefinition> task;ComPtr<IPrincipal> principal;
    if(FAILED(registered->get_Definition(&task))||FAILED(task->get_Principal(&principal)))return false;
    TASK_RUNLEVEL_TYPE level=TASK_RUNLEVEL_LUA;
    TASK_LOGON_TYPE logon=TASK_LOGON_NONE;
    if(FAILED(principal->get_RunLevel(&level))||FAILED(principal->get_LogonType(&logon))||
       level!=TASK_RUNLEVEL_HIGHEST||logon!=TASK_LOGON_INTERACTIVE_TOKEN)return false;
    ComPtr<IActionCollection> actions;ComPtr<IAction> action;ComPtr<IExecAction> exec;
    if(FAILED(task->get_Actions(&actions))||FAILED(actions->get_Item(1,&action))||FAILED(action.As(&exec)))return false;
    BSTR path=nullptr;
    hr=exec->get_Path(&path);
    bool matches=SUCCEEDED(hr)&&path&&!_wcsicmp(path,exe_path().c_str());
    SysFreeString(path);
    return matches;
}

bool set_autostart(bool enabled,std::wstring& error){
    LogTrace trace;
    error.clear();
    logf(L"管理员开机启动请求到达：启用=%d 程序=%s",(int)enabled,exe_path().c_str());
    if(!process_is_elevated(GetCurrentProcess()))
        return failed(L"管理员权限检查",E_ACCESSDENIED,error);
    std::wstring sid;
    HRESULT hr=user_sid(sid);
    if(FAILED(hr))return failed(L"读取当前用户",hr,error);
    ComPtr<ITaskService> service;ComPtr<ITaskFolder> folder;
    hr=scheduler(service,folder);
    if(FAILED(hr))return failed(L"连接任务计划程序",hr,error);
    std::wstring name=task_name(sid);
    if(enabled){
        ComPtr<ITaskDefinition> definition;
        hr=create_autostart_definition(service.Get(),sid,exe_path(),&definition);
        if(FAILED(hr))return failed(L"创建登录启动定义",hr,error);
        logf(L"管理员登录任务定义完成：任务=%s 最高权限=是 交互桌面=是 不保存密码=是",name.c_str());
        ComPtr<IRegisteredTask> registered;
        hr=folder->RegisterTaskDefinition(_bstr_t(name.c_str()),definition.Get(),TASK_CREATE_OR_UPDATE,
            _variant_t(sid.c_str()),_variant_t(),TASK_LOGON_INTERACTIVE_TOKEN,_variant_t(),&registered);
        if(hr!=S_OK)return failed(L"注册管理员登录任务",hr,error);
    }else{
        hr=folder->DeleteTask(_bstr_t(name.c_str()),0);
        if(FAILED(hr)&&hr!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return failed(L"删除管理员登录任务",hr,error);
    }
    hr=remove_legacy_run();
    if(FAILED(hr))return failed(L"移除旧版普通权限启动项",hr,error);
    logf(L"管理员开机启动返回：成功 启用=%d 任务=%s，旧版 Run 启动项已移除",
         (int)enabled,name.c_str());
    return true;
}

bool migrate_legacy_autostart(std::wstring& error){
    error.clear();
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,RUN_KEY,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return true;
    DWORD size=0;
    LSTATUS status=RegQueryValueExW(key,RUN_VALUE,nullptr,nullptr,nullptr,&size);
    RegCloseKey(key);
    if(status!=ERROR_SUCCESS)return true;
    LogTrace trace;
    logf(L"检测到旧版普通权限开机启动设置，转换为管理员登录任务");
    return set_autostart(true,error);
}
