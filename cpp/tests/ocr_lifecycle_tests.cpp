#include "../src/ocr.h"
#include "../src/fast_ocr.h"
#include "../src/settings.h"
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <iostream>

static int passed=0,failed=0;
static void check(bool ok,const char* name){
    std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;
    logf(L"模型生命周期回归：%s 结果=%s",u8_to_w(name).c_str(),ok?L"通过":L"失败");
    ok?++passed:++failed;
}
static DWORD own_worker(){
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return 0;
    PROCESSENTRY32W entry{sizeof(entry)};DWORD pid=0;
    if(Process32FirstW(snapshot,&entry))do{
        if(entry.th32ParentProcessID!=GetCurrentProcessId()||wcscmp(entry.szExeFile,L"ocr_ncnn.exe"))continue;
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
        DWORD code=0;
        if(process&&GetExitCodeProcess(process,&code)&&code==STILL_ACTIVE)pid=entry.th32ProcessID;
        if(process)CloseHandle(process);
    }while(Process32NextW(snapshot,&entry)&&!pid);
    CloseHandle(snapshot);return pid;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3)return 2;
    auto root=std::filesystem::path(argv[2])/(L"session-"+std::to_wstring(GetCurrentProcessId()));
    auto logs=root/L"logs",runtime=root/L"weijie"/L"ocr-ncnn";
    std::filesystem::create_directories(logs);std::filesystem::create_directories(runtime/L"models");
    SetEnvironmentVariableW(L"APPDATA",root.c_str());SetEnvironmentVariableW(L"LOCALAPPDATA",root.c_str());
    LogTrace trace;
    auto traceId=L"shot-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(trace.id());
    SetEnvironmentVariableW(L"WEIJIE_TEST_TRACE_ID",traceId.c_str());
    SetEnvironmentVariableW(L"WEIJIE_TEST_LOG_DIR",logs.c_str());
    std::filesystem::copy_file(argv[1],runtime/L"ocr_ncnn.exe",std::filesystem::copy_options::overwrite_existing);
    for(auto name:{L"ncnn.dll",L"opencv_world4110.dll",L"config-cpu.json",L"config-gpu.json",
        L"models/PP_OCRv6_small_det.param",L"models/PP_OCRv6_small_det.bin",
        L"models/PP_OCRv6_small_rec.param",L"models/PP_OCRv6_small_rec.bin",
        L"models/PP_LCNet_x0_25_textline_ori.param",L"models/PP_LCNet_x0_25_textline_ori.bin",
        L"models/ppocr_keys_v6.txt"})std::ofstream(runtime/name)<<"fixture";
    auto startedName=L"Local\\weijie-ocr-started-"+std::to_wstring(GetCurrentProcessId());
    auto releaseName=L"Local\\weijie-ocr-release-"+std::to_wstring(GetCurrentProcessId());
    HANDLE started=CreateEventW(nullptr,TRUE,FALSE,startedName.c_str());
    HANDLE release=CreateEventW(nullptr,TRUE,TRUE,releaseName.c_str());
    SetEnvironmentVariableW(L"WEIJIE_TEST_STARTED",startedName.c_str());
    SetEnvironmentVariableW(L"WEIJIE_TEST_RELEASE",releaseName.c_str());
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token;
    Gdiplus::GdiplusStartup(&token,&input,nullptr);
    Gdiplus::Bitmap image(160,80,PixelFormat32bppRGB);
    Settings settings;settings.ocr_enabled=true;settings.ocr_engine=L"ppocr";settings.ocr_backend=L"gpu";
    check(save_settings(settings),"enable OCR without preloading a model");
    check(own_worker()==0,"enabling is lazy");
    std::wstring text,error;
    check(fast_ocr_recognize(&image,text,error)&&text==L"fixture text","load resident OCR worker on first request");
    DWORD first=own_worker();
    HANDLE old=OpenProcess(SYNCHRONIZE,FALSE,first);
    check(first!=0&&old!=nullptr,"resident model process is owned by this test");
    settings.ocr_enabled=false;
    check(save_settings(settings),"save master switch off");
    check(old&&WaitForSingleObject(old,0)==WAIT_OBJECT_0,"idle model process exits when disabled");
    check(fast_ocr_initializing(),"cached initialized state is cleared");
    check(!fast_ocr_recognize(&image,text,error)&&error.find(L"主开关已关闭")!=std::wstring::npos,
          "disabled direct OCR cannot respawn worker");
    check(!ocr_recognize(&image,text,error)&&error.find(L"主开关已关闭")!=std::wstring::npos,
          "disabled OCR wrapper cannot load a model");
    check(own_worker()==0,"no model process remains after disabling");
    settings.ocr_enabled=true;save_settings(settings);
    check(own_worker()==0,"re-enable does not eagerly allocate model resources");
    check(fast_ocr_recognize(&image,text,error),"next recognition reloads model");
    DWORD second=own_worker();
    check(second&&second!=first,"re-enable starts a fresh model process");
    if(old)CloseHandle(old);
    check(!ocr_reclaim_idle(GetTickCount64()+25ULL*60*60000)&&own_worker()==second,
          "off keeps model resident even after simulated 25 hours");
    for(int minutes:{1,10,60,720,1440}){
        settings.ocr_idle_minutes=minutes;save_settings(settings);
        ULONGLONG now=GetTickCount64(),limit=(ULONGLONG)minutes*60000;
        DWORD resident=own_worker();
        HANDLE model=OpenProcess(SYNCHRONIZE,FALSE,resident);
        check(!ocr_reclaim_idle(now+limit-1000)&&own_worker()==resident,
              "model remains resident before selected idle deadline");
        {
            OcrActivity preparing;
            check(!ocr_reclaim_idle(now+limit+1000)&&own_worker()==resident,
                  "request preparation protects model even without inference lock");
        }
        check(ocr_reclaim_idle(GetTickCount64()+limit+1000),
              "selected idle deadline reclaims model");
        check(model&&WaitForSingleObject(model,0)==WAIT_OBJECT_0&&own_worker()==0&&fast_ocr_initializing(),
              "idle reclaim exits process and clears cached initialization");
        if(model)CloseHandle(model);
        check(ocr_is_enabled()&&load_settings().ocr_enabled&&own_worker()==0,
              "idle reclaim preserves master switch without preloading");
        check(fast_ocr_recognize(&image,text,error)&&own_worker()!=resident,
              "recognition lazily reloads a fresh model after idle reclaim");
    }
    settings.ocr_idle_minutes=1;save_settings(settings);
    second=own_worker();
    HANDLE busy=OpenProcess(SYNCHRONIZE,FALSE,second);
    ResetEvent(started);ResetEvent(release);
    std::atomic<bool> done{false};bool recognized=false;
    std::thread request([&]{
        LogTrace operation(trace.id());
        recognized=fast_ocr_recognize(&image,text,error);
        done=true;
    });
    check(WaitForSingleObject(started,5000)==WAIT_OBJECT_0&&!done,"recognition is actively holding inference lock");
    check(!ocr_reclaim_idle(GetTickCount64()+25ULL*60*60000)&&!done&&own_worker()==second,
          "idle timeout never interrupts active inference");
    SetEvent(release);request.join();
    check(done&&recognized&&!ocr_reclaim_idle(GetTickCount64()+59000)&&own_worker()==second,
          "inference completion restarts idle countdown");
    ULONGLONG completed=GetTickCount64();
    Sleep(20);
    Settings other=settings;other.jpeg_quality=81;save_settings(other);
    check(ocr_reclaim_idle(completed+60000)&&ocr_is_enabled(),
          "unrelated settings save preserves elapsed idle countdown");
    if(busy)CloseHandle(busy);
    fast_ocr_recognize(&image,text,error);
    second=own_worker();busy=OpenProcess(SYNCHRONIZE,FALSE,second);
    ResetEvent(started);ResetEvent(release);done=false;recognized=false;
    request=std::thread([&]{
        LogTrace operation(trace.id());
        recognized=fast_ocr_recognize(&image,text,error);
        done=true;
    });
    check(WaitForSingleObject(started,5000)==WAIT_OBJECT_0&&!done,"recognition is active before forced master shutdown");
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit));
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION extra{};
    std::wstring command=L"\""+std::wstring(argv[1])+L"\" --hold";
    bool extraStarted=CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&extra)!=FALSE;
    bool tracked=extraStarted&&AssignProcessToJobObject(job,extra.hProcess)&&ocr_track_process(job,extra.hProcess);
    check(tracked,"register an additional one-shot model process");
    if(tracked)ResumeThread(extra.hThread);
    ULONGLONG stop=GetTickCount64();
    settings.ocr_enabled=false;save_settings(settings);
    ULONGLONG elapsed=GetTickCount64()-stop;
    check(elapsed<3000,"disable does not wait for busy inference to finish");
    check(busy&&WaitForSingleObject(busy,0)==WAIT_OBJECT_0,"busy resident model process is terminated");
    check(tracked&&WaitForSingleObject(extra.hProcess,0)==WAIT_OBJECT_0,"one-shot model process is also terminated");
    SetEvent(release);
    request.join();
    check(done&&!recognized,"active recognition is interrupted instead of completing");
    check(own_worker()==0,"busy shutdown leaves no resident model process");
    if(extraStarted){
        if(!tracked)TerminateProcess(extra.hProcess,2);
        ocr_forget_process(job);
        CloseHandle(extra.hThread);CloseHandle(extra.hProcess);
    }
    CloseHandle(job);if(busy)CloseHandle(busy);
    ocr_set_enabled(false);fast_ocr_shutdown();
    CloseHandle(started);CloseHandle(release);
    std::cout<<"TOTAL "<<passed<<" passed, "<<failed<<" failed"<<std::endl;
    return failed?1:0;
}
