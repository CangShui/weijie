#include "ocr.h"
#include "settings.h"
#include "fast_ocr.h"

#include <vector>
#include <mutex>

namespace {
struct ModelProcess { HANDLE sourceJob,job,process; };
std::mutex modelMutex;
std::vector<ModelProcess> modelProcesses;
std::atomic<bool> modelEnabled{false};
int idleMinutes=0;
unsigned activeRequests=0;
ULONGLONG lastActivity=GetTickCount64();
}

bool ocr_is_enabled(){return modelEnabled.load();}

OcrActivity::OcrActivity(){
    std::lock_guard<std::mutex> lock(modelMutex);
    ++activeRequests;
    logf(L"OCR 调用开始：活动调用=%u，排队、加载和推理期间不自动回收",activeRequests);
}
OcrActivity::~OcrActivity(){
    std::lock_guard<std::mutex> lock(modelMutex);
    --activeRequests;
    lastActivity=GetTickCount64();
    logf(L"OCR 调用结束：剩余调用=%u 空闲计时起点=%llu 超时分钟=%d",
         activeRequests,lastActivity,idleMinutes);
}
void ocr_set_idle_timeout(int minutes){
    std::lock_guard<std::mutex> lock(modelMutex);
    if(idleMinutes==minutes)return;
    logf(L"OCR 空闲回收设置生效：原分钟=%d 新分钟=%d（0=off），不改变主开关或预加载模型",
         idleMinutes,minutes);
    idleMinutes=minutes;
}
bool ocr_idle_expired(ULONGLONG now){
    std::lock_guard<std::mutex> lock(modelMutex);
    if(!modelEnabled||!idleMinutes||activeRequests||now<lastActivity)return false;
    ULONGLONG elapsed=now-lastActivity,limit=(ULONGLONG)idleMinutes*60000;
    if(elapsed<limit)return false;
    logf(L"OCR 空闲超时：当前=%llu 上次调用结束=%llu 空闲毫秒=%llu 阈值=%d分钟×60000=%llu，活动调用=0，允许回收",
         now,lastActivity,elapsed,idleMinutes,limit);
    return true;
}

bool ocr_track_process(HANDLE job,HANDLE process){
    std::lock_guard<std::mutex> lock(modelMutex);
    if(!modelEnabled){
        logf(L"模型进程注册被拒绝：识别主开关已关闭，不允许加载模型");
        return false;
    }
    ModelProcess item{job,nullptr,nullptr};
    HANDLE self=GetCurrentProcess();
    if(!DuplicateHandle(self,job,self,&item.job,0,FALSE,DUPLICATE_SAME_ACCESS)||
       !DuplicateHandle(self,process,self,&item.process,0,FALSE,DUPLICATE_SAME_ACCESS)){
        DWORD error=GetLastError();
        if(item.job)CloseHandle(item.job);
        if(item.process)CloseHandle(item.process);
        logf(L"模型进程注册失败：无法保留停止句柄 error=%lu",error);
        return false;
    }
    modelProcesses.push_back(item);
    logf(L"模型进程已纳入主开关管理：pid=%lu",GetProcessId(process));
    return true;
}

void ocr_forget_process(HANDLE job){
    std::lock_guard<std::mutex> lock(modelMutex);
    auto found=std::find_if(modelProcesses.begin(),modelProcesses.end(),
        [job](const ModelProcess& item){return item.sourceJob==job;});
    if(found==modelProcesses.end())return;
    CloseHandle(found->job);CloseHandle(found->process);
    modelProcesses.erase(found);
}

void ocr_set_enabled(bool enabled){
    LogTrace trace;
    logf(L"识别主开关请求到达：启用=%d",(int)enabled);
    bool unloaded=true;
    {
        std::lock_guard<std::mutex> lock(modelMutex);
        modelEnabled.store(enabled);
        if(!enabled){
            for(const auto& item:modelProcesses){
                DWORD pid=GetProcessId(item.process);
                if(WaitForSingleObject(item.process,0)!=WAIT_OBJECT_0){
                    BOOL stopped=TerminateJobObject(item.job,ERROR_CANCELLED);
                    DWORD error=stopped?ERROR_SUCCESS:GetLastError();
                    DWORD waited=WaitForSingleObject(item.process,3000);
                    unloaded=unloaded&&waited==WAIT_OBJECT_0;
                    logf(L"识别主开关卸载模型：pid=%lu 终止成功=%d error=%lu 进程已退出=%d",
                         pid,(int)stopped,error,(int)(waited==WAIT_OBJECT_0));
                }
            }
        }
    }
    // 不等待正在推理的线程持有的互斥锁；模型进程已由上面的独立句柄终止。
    if(!enabled)fast_ocr_unload();
    logf(L"识别主开关返回：启用=%d 模型进程已停止=%d；开启仅允许按需加载",
         (int)enabled,(int)(!enabled&&unloaded));
}

bool ocr_recognize(Gdiplus::Bitmap* bmp, std::wstring& text, std::wstring& err, const std::atomic<bool>* cancel) {
    LogTrace trace;
    OcrActivity activity;
    text.clear();
    err.clear();
    if (!bmp) { err = L"没有可识别的图像";logf(L"识别请求被拒绝：输入图像为空"); return false; }

    Settings s = load_settings();
    if(!s.ocr_enabled||!ocr_is_enabled()){
        err=L"识别主开关已关闭，请开启后再识别图片";
        logf(L"识别请求被拒绝：主开关已关闭，不启动或重新加载模型");
        return false;
    }
    logf(L"识别引擎请求到达：引擎=%s 图像=%ux%u 识别按钮显示=%d",
         s.ocr_engine.c_str(),bmp->GetWidth(),bmp->GetHeight(),(int)s.ocr_enabled);
    bool ok=fast_ocr_recognize(bmp,text,err,cancel);
    if(!ok&&err.empty())err=L"PP-OCRv6 识别失败";
    logf(L"识别引擎返回：成功=%d 字符数=%zu 错误=%s",(int)ok,text.size(),err.c_str());
    return ok;
}
