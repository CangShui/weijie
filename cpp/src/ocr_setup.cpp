#include "ocr_setup.h"
#include "settings.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <stdexcept>
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"bcrypt.lib")
namespace {
struct Asset { const wchar_t* name; unsigned long long size; const char* hash; };
const Asset assets[] = {
 {L"HunyuanOCR-Q8_0.gguf",577949408ull,"cdafc794cafeae377868d7a40a70e282a737e39abe77c0d8b73614447b364a21"},
 {L"mmproj-HunyuanOCR-Q8_0.gguf",732938240ull,"b77913164ff73d4c0dc4d994e236ed72bacbbe5c5db1ec9b2828627b46c32804"}
};
const wchar_t* base = L"https://huggingface.co/ggml-org/HunyuanOCR-GGUF/resolve/8e070c9ad79e4ca97a9b4daa2f1ce17e8759afb1/";
std::thread worker;
std::atomic<bool> cancel{false};
std::mutex stateMutex;
OcrDownloadState state;
void update(std::wstring text,int percent=-1) {
 std::lock_guard<std::mutex> guard(stateMutex); state.text=std::move(text);
 if(percent>=0) state.percent=percent;
}
unsigned long long size_of(const std::wstring& p) {
 WIN32_FILE_ATTRIBUTE_DATA fd{};
 if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&fd)) return 0;
 return ((unsigned long long)fd.nFileSizeHigh<<32)|fd.nFileSizeLow;
}
std::string stamp(const std::wstring& p,const Asset& a) {
 WIN32_FILE_ATTRIBUTE_DATA fd{};
 if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&fd) || size_of(p)!=a.size) return {};
 return std::string(a.hash)+":"+std::to_string(fd.ftLastWriteTime.dwHighDateTime)+":"+std::to_string(fd.ftLastWriteTime.dwLowDateTime);
}
bool marked(const std::wstring& p,const Asset& a) {
 std::ifstream f(std::filesystem::path(p+L".verified")); std::string s; std::getline(f,s);
 auto expected=stamp(p,a); return !expected.empty() && s==expected;
}
bool verify(const std::wstring& p,const Asset& a) {
 if(size_of(p)!=a.size) return false;
 BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{};
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return false;
 if(BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)<0) { BCryptCloseAlgorithmProvider(alg,0); return false; }
 std::ifstream f(std::filesystem::path(p),std::ios::binary);
 std::vector<char> buf(1024*1024); bool ok=bool(f);
 while(f && !cancel) {
  f.read(buf.data(),buf.size()); auto n=f.gcount();
  if(n>0 && BCryptHashData(hash,(PUCHAR)buf.data(),(ULONG)n,0)<0) { ok=false; break; }
 }
 unsigned char digest[32];
 ok=ok && f.eof() && !cancel && BCryptFinishHash(hash,digest,32,0)>=0;
 BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg,0);
 if(!ok) return false;
 std::string actual; const char* hex="0123456789abcdef";
 for(auto b:digest){actual+=hex[b>>4];actual+=hex[b&15];}
 return actual==a.hash;
}
struct Http { HINTERNET h{}; ~Http(){if(h)WinHttpCloseHandle(h);} operator HINTERNET()const{return h;} };
bool fetch(const std::wstring& path,const Asset& asset,int index) {
 auto offset=size_of(path); if(offset>asset.size) offset=0;
 const std::wstring url=std::wstring(base)+asset.name;
 URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=(DWORD)-1;u.dwUrlPathLength=(DWORD)-1;
 if(!WinHttpCrackUrl(url.c_str(),0,0,&u))return false;
 std::wstring host(u.lpszHostName,u.dwHostNameLength),resource(u.lpszUrlPath,u.dwUrlPathLength);
 Http session{WinHttpOpen(L"weijie/1.2",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};
 if(!session.h)return false;
 WinHttpSetTimeouts(session,15000,15000,15000,15000);
 Http conn{WinHttpConnect(session,host.c_str(),u.nPort,0)};
 Http req{WinHttpOpenRequest(conn,L"GET",resource.c_str(),nullptr,nullptr,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
 if(!req.h)return false;
 std::wstring range=L"Range: bytes="+std::to_wstring(offset)+L"-\r\n";
 if(!WinHttpSendRequest(req,offset?range.c_str():nullptr,offset?(DWORD)-1:0,nullptr,0,0,0)||!WinHttpReceiveResponse(req,nullptr))return false;
 DWORD status=0,len=sizeof(status);
 WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&len,nullptr);
 if(status!=200 && status!=206)return false;
 if(status==206){
  wchar_t value[128]{};len=sizeof(value);
  if(!WinHttpQueryHeaders(req,WINHTTP_QUERY_CONTENT_RANGE,nullptr,value,&len,nullptr))return false;
  std::wstring prefix=L"bytes "+std::to_wstring(offset)+L"-";
  if(std::wstring(value).find(prefix)!=0)return false;
 } else offset=0; // Server ignored Range: restart instead of appending duplicate data.
 std::ofstream f(std::filesystem::path(path),std::ios::binary|(offset?std::ios::app:std::ios::trunc));
 if(!f)return false;
 char buf[65536];DWORD got=0;
 while(!cancel && offset<asset.size){
  if(!WinHttpReadData(req,buf,sizeof(buf),&got)||!got)break;
  if(offset+got>asset.size)return false;
  f.write(buf,got);if(!f)return false;offset+=got;
  int pc=(int)((index==0?offset:assets[0].size+offset)*100/(assets[0].size+assets[1].size));
  update(std::wstring(asset.name)+L"  "+std::to_wstring(offset/1048576)+L" / "+std::to_wstring(asset.size/1048576)+L" MB",pc);
 }
 f.close();return !cancel && offset==asset.size;
}
}
std::wstring hunyuan_ocr_dir(){
 auto dir=load_settings().ocr_model_dir;
 if(dir.empty())dir=localappdata_dir()+L"\\hunyuan-ocr";
 return dir;
}
std::wstring hunyuan_runtime_dir(bool gpu){
 auto exe=exe_path();auto pos=exe.find_last_of(L"\\/");auto root=exe.substr(0,pos);
 for(const auto& d:{root+(gpu?L"\\ocr-runtime-gpu":L"\\ocr-runtime"),
                    hunyuan_ocr_dir()+(gpu?L"\\runtime-gpu":L"\\runtime")})
  if(GetFileAttributesW((d+L"\\llama-mtmd-cli.exe").c_str())!=INVALID_FILE_ATTRIBUTES)return d;
 return {};
}
bool hunyuan_runtime_ready(){
 for(bool gpu:{false,true}){
  auto dir=hunyuan_runtime_dir(gpu);if(dir.empty())continue;
  bool ok=true;
  for(auto f:{L"llama-mtmd-cli.exe",L"llama.dll",L"mtmd.dll",L"llama-common.dll",L"ggml.dll",L"ggml-base.dll"})
   if(!size_of(dir+L"\\"+f)){ok=false;break;}
  if(ok)return true;
 }
 return false;
}
bool hunyuan_model_ready(){
 if(!hunyuan_runtime_ready())return false;
 for(const auto& a:assets)if(!marked(hunyuan_ocr_dir()+L"\\"+a.name,a))return false;
 return true;
}
bool hunyuan_fast_model_ready(){
 if(!hunyuan_runtime_ready())return false;
 auto dir=hunyuan_ocr_dir();
 return size_of(dir+L"\\HunyuanOCR-Q3_K_S.gguf")>=285219968ull &&
        size_of(dir+L"\\mmproj-HunyuanOCR-Q8_0.gguf")>=650ull*1024*1024;
}
std::wstring hunyuan_model_status_text(){
 if(!hunyuan_runtime_ready())return L"缺少 ocr-runtime，请使用完整程序目录";
 if(hunyuan_model_ready())return L"模型 SHA-256 校验通过，可离线识别";
 return L"未安装或未校验。下载约 1.31 GB，可取消并续传";
}
OcrDownloadState hunyuan_download_state(){std::lock_guard<std::mutex> g(stateMutex);return state;}
void hunyuan_cancel_download(){cancel=true;}
void hunyuan_download_shutdown(){cancel=true;if(worker.joinable())worker.join();}
void hunyuan_download_models_async(){
 if(hunyuan_download_state().busy)return;
 if(worker.joinable())worker.join();
 cancel=false;{std::lock_guard<std::mutex> g(stateMutex);state={true,0,L"准备下载 / 校验…"};}
 worker=std::thread([]{
  try{
   auto dir=hunyuan_ocr_dir();std::filesystem::create_directories(dir);
   if(!hunyuan_runtime_ready())throw std::runtime_error("missing runtime");
   for(int i=0;i<2;++i){
    const auto& a=assets[i];auto dest=dir+L"\\"+a.name;
    if(marked(dest,a))continue;
    update(L"正在校验 "+std::wstring(a.name));
    if(!verify(dest,a)){
     if(cancel)throw std::runtime_error("cancel");
     auto part=dest+L".part";bool ok=false;
     for(int attempt=0;attempt<3 && !cancel;++attempt){
      if(size_of(part)!=a.size && !fetch(part,a,i))continue;
      update(L"正在校验 SHA-256…");
      if(verify(part,a)){ok=true;break;}
      if(!cancel)DeleteFileW(part.c_str()); // Only this task's failed staging file.
     }
     if(!ok)throw std::runtime_error("download/hash failure");
     if(!MoveFileExW(part.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("move failed");
    }
    std::ofstream marker(std::filesystem::path(dest+L".verified"));marker<<stamp(dest,a);marker.close();
   }
   update(L"模型校验完成。勾选启用并保存后显示识别按钮。",100);
  }catch(...){update(cancel?L"已取消，部分下载保留，可继续下载":L"下载或校验失败。请检查网络、磁盘空间后重试。");}
  std::lock_guard<std::mutex> g(stateMutex);state.busy=false;
 });
}
