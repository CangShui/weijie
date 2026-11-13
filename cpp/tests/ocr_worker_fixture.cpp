#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static std::wstring env(const wchar_t* name){
    wchar_t value[2048]{};GetEnvironmentVariableW(name,value,2048);return value;
}
static void audit(const char* message){
    auto path=std::filesystem::path(env(L"WEIJIE_TEST_LOG_DIR"))/
        (L"fixture-"+std::to_wstring(GetCurrentProcessId())+L".log");
    char trace[128]{};GetEnvironmentVariableA("WEIJIE_TEST_TRACE_ID",trace,128);
    std::ofstream out(path,std::ios::app);
    out<<"traceId="<<trace<<" pid="<<GetCurrentProcessId()<<" "<<message<<"\n";
}
int main(int argc,char**){
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    HANDLE started=OpenEventW(EVENT_MODIFY_STATE,FALSE,env(L"WEIJIE_TEST_STARTED").c_str());
    HANDLE release=OpenEventW(SYNCHRONIZE,FALSE,env(L"WEIJIE_TEST_RELEASE").c_str());
    if(!started||!release){audit("测试事件不可用，未开始处理");return 2;}
    audit("测试模型进程已启动");
    if(argc>1){
        SetEvent(started);WaitForSingleObject(release,15000);
    }else{
        std::cout<<"READY\n"<<std::flush;
        std::string device,image;
        while(std::getline(std::cin,device)&&std::getline(std::cin,image)){
            audit("识别请求已到达，正在等待测试计算结束");
            SetEvent(started);
            if(WaitForSingleObject(release,15000)!=WAIT_OBJECT_0){
                std::cout<<"ERR 7\ntimeout\n"<<std::flush;break;
            }
            std::string text="fixture text";
            std::cout<<"OK "<<text.size()<<"\n"<<text<<"\n"<<std::flush;
            audit("识别请求已返回成功");
        }
    }
    audit("测试模型进程正常退出");
    CloseHandle(started);CloseHandle(release);
    return 0;
}
