#include "../src/settings.h"
#include "../src/output.h"
#include "../src/longshot.h"
#include "../src/ocr.h"
#include "../src/fast_ocr.h"
#include "../src/ocr_setup.h"
#include "../src/hotkey_input.h"
#include "../src/desktop_duplication.h"
#include "../src/autostart.h"
#include "../src/capture_frame_wait.h"
#include <wrl/client.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
static int count=0;
void check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;if(!ok)exit(1);++count;}
std::string head(const std::wstring&p,int n){std::ifstream f(std::filesystem::path(p),std::ios::binary);std::string s(n,'\0');f.read(s.data(),n);return s;}
static DWORD own_ocr_pid(){
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return 0;
 PROCESSENTRY32W entry{sizeof(entry)};DWORD pid=0;
 if(Process32FirstW(snapshot,&entry))do{
  if(entry.th32ParentProcessID!=GetCurrentProcessId()||wcscmp(entry.szExeFile,L"ocr_ncnn.exe"))continue;
  HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);DWORD code=0;
  if(process&&GetExitCodeProcess(process,&code)&&code==STILL_ACTIVE)pid=entry.th32ProcessID;
  if(process)CloseHandle(process);
 }while(Process32NextW(snapshot,&entry)&&!pid);
 CloseHandle(snapshot);return pid;
}
int wmain(int argc,wchar_t**argv){
 wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);auto dir=std::wstring(temp)+L"weijie-core-tests-"+std::to_wstring(GetCurrentProcessId());std::filesystem::create_directories(dir);
 // Isolated settings: never overwrite user configuration during regression tests.
 SetEnvironmentVariableW(L"APPDATA",dir.c_str());
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 CoInitializeSecurity(nullptr,-1,nullptr,nullptr,RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
  RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE,nullptr);
 Gdiplus::GdiplusStartupInput input;ULONG_PTR token;Gdiplus::GdiplusStartup(&token,&input,nullptr);
 check(load_settings().ocr_idle_minutes==0,"missing OCR idle timeout defaults to off");
 Settings s;s.hotkey=L"Ctrl+Alt+K";s.hotkey_enabled=false;s.save_dir=L"C:\\测试目录\\Screens";s.image_format=L"bmp";s.jpeg_quality=77;s.ocr_enabled=false;s.translate_enabled=false;s.stroke_width=8;s.text_size=32;
 check(save_settings(s),"atomic settings write");auto loaded=load_settings();
 check(loaded.save_dir==s.save_dir,"Unicode path round trip");check(!loaded.hotkey_enabled,"monitor switch round trip");check(!loaded.ocr_enabled,"OCR opt-out persists");check(!loaded.translate_enabled,"translation opt-out persists");
 check(loaded.stroke_width==8&&loaded.text_size==32,"annotation settings persist");
 check(loaded.jpeg_quality==77&&loaded.image_format==L"bmp","encoding settings persist");
 for(int minutes:OCR_IDLE_TIMEOUT_MINUTES){
  s.ocr_idle_minutes=minutes;
  check(save_settings(s)&&load_settings().ocr_idle_minutes==minutes,"OCR idle timeout option round trip");
 }
 s.ocr_idle_minutes=2;
 check(!save_settings(s)&&load_settings().ocr_idle_minutes==1440,"invalid OCR idle option does not overwrite settings");
 s.ocr_idle_minutes=0;save_settings(s);
 {
  LogTrace trace;
  using Microsoft::WRL::ComPtr;
  ComPtr<ITaskService> service;
  check(SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&service))),"create Task Scheduler COM service");
  VARIANT empty;VariantInit(&empty);
  check(SUCCEEDED(service->Connect(empty,empty,empty,empty)),"connect local Task Scheduler without changing tasks");
  const std::wstring sid=L"S-1-5-21-100-200-300-1001";
  const std::wstring executable=L"C:\\截图 & 工具\\微截.exe";
  ComPtr<ITaskDefinition> task;
  check(SUCCEEDED(create_autostart_definition(service.Get(),sid,executable,&task)),
        "build elevated logon definition without registering a machine task");
  ComPtr<IPrincipal> principal;task->get_Principal(&principal);
  TASK_RUNLEVEL_TYPE level=TASK_RUNLEVEL_LUA;TASK_LOGON_TYPE logon=TASK_LOGON_NONE;
  check(SUCCEEDED(principal->get_RunLevel(&level))&&level==TASK_RUNLEVEL_HIGHEST,
        "autostart uses highest privileges");
  check(SUCCEEDED(principal->get_LogonType(&logon))&&logon==TASK_LOGON_INTERACTIVE_TOKEN,
        "autostart uses logged-on desktop without storing a password");
  BSTR value=nullptr;principal->get_UserId(&value);
  check(value&&sid==value,"autostart principal is current user");SysFreeString(value);
  ComPtr<ITriggerCollection> triggers;ComPtr<ITrigger> trigger;ComPtr<ILogonTrigger> login;
  task->get_Triggers(&triggers);LONG triggerCount=0;triggers->get_Count(&triggerCount);
  check(triggerCount==1&&SUCCEEDED(triggers->get_Item(1,&trigger))&&
        SUCCEEDED(trigger.As(&login)),"one logon trigger");
  value=nullptr;login->get_UserId(&value);
  check(value&&sid==value,"logon trigger targets the same user");SysFreeString(value);
  ComPtr<IActionCollection> actions;ComPtr<IAction> action;ComPtr<IExecAction> exec;
  task->get_Actions(&actions);actions->get_Item(1,&action);action.As(&exec);
  value=nullptr;exec->get_Path(&value);
  check(value&&executable==value,"autostart preserves Unicode and spaces in executable path");SysFreeString(value);
  value=nullptr;exec->get_WorkingDirectory(&value);
  check(value&&std::filesystem::path(executable).parent_path().wstring()==value,
        "autostart working directory is the application directory");SysFreeString(value);
  value=nullptr;exec->get_Arguments(&value);
  check(value&&std::wstring(value)==L"--autostart","autostart source is recorded in arguments");SysFreeString(value);
  ComPtr<ITaskSettings> options;task->get_Settings(&options);
  VARIANT_BOOL battery=VARIANT_TRUE,stopOnBattery=VARIANT_TRUE;
  options->get_DisallowStartIfOnBatteries(&battery);options->get_StopIfGoingOnBatteries(&stopOnBattery);
  check(battery==VARIANT_FALSE&&stopOnBattery==VARIANT_FALSE,"autostart remains available on battery power");
  value=nullptr;options->get_ExecutionTimeLimit(&value);
  check(value&&std::wstring(value)==L"PT0S","resident screenshot app has no scheduled runtime limit");SysFreeString(value);
  TASK_INSTANCES_POLICY instances=TASK_INSTANCES_PARALLEL;options->get_MultipleInstances(&instances);
  check(instances==TASK_INSTANCES_IGNORE_NEW,"scheduler does not launch duplicate resident instances");
  logf(L"管理员开机启动定义回归完成：未注册或删除本机计划任务");
  auto app=std::filesystem::path(exe_path()).parent_path()/L"微截.exe";
  HMODULE binary=LoadLibraryExW(app.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  check(binary!=nullptr,"read application resources without launching elevated application");
  HRSRC resource=FindResourceW(binary,MAKEINTRESOURCEW(1),MAKEINTRESOURCEW(24));
  check(resource!=nullptr,"application has embedded UAC manifest");
  HGLOBAL memory=LoadResource(binary,resource);
  const char* bytes=(const char*)LockResource(memory);
  std::string manifest(bytes,SizeofResource(binary,resource));
  check(manifest.find("requestedExecutionLevel")!=std::string::npos&&
        manifest.find("requireAdministrator")!=std::string::npos,
        "application requests administrator privileges by default");
  FreeLibrary(binary);
 }
 UINT mods,vk;check(resolve_hotkey(L"Ctrl+Alt+K",mods,vk)&&mods==(MOD_CONTROL|MOD_ALT)&&vk=='K',"custom hotkey");
 check(resolve_hotkey(L"F24",mods,vk)&&vk==VK_F24,"F24 hotkey");check(resolve_hotkey(L"PrintScreen",mods,vk)&&vk==VK_SNAPSHOT,"PrintScreen hotkey");
 check(!resolve_hotkey(L"A",mods,vk),"reject plain letter");check(!resolve_hotkey(L"Ctrl+F25",mods,vk),"reject invalid F key");check(!resolve_hotkey(L"Ctrl+Ctrl+A",mods,vk),"reject duplicate modifier");
 {
  RawHotkeyState keys;
  auto key=[&](USHORT vk,USHORT flags=0,USHORT scan=0){
   RAWKEYBOARD input{};input.VKey=vk;input.Flags=flags;input.MakeCode=scan;
   return keys.update(input,MOD_ALT,'A');
  };
  check(!key(VK_MENU),"raw modifier alone does not trigger");
  check(key('A'),"raw Alt+A triggers without async key state");
  check(!key('A'),"raw held key does not repeat");
  check(!key('A',RI_KEY_BREAK)&&key('A'),"raw key release rearms");
  key('A',RI_KEY_BREAK);key(VK_MENU,RI_KEY_BREAK);
  check(!key('A'),"raw missing modifier rejected");key('A',RI_KEY_BREAK);
  key(VK_MENU,RI_KEY_E0);key(VK_CONTROL);
  check(!key('A'),"raw extra modifier rejected");key('A',RI_KEY_BREAK);
  key(VK_CONTROL,RI_KEY_BREAK);
  check(key('A'),"raw right Alt recognized");key('A',RI_KEY_BREAK);
  key(VK_LWIN);
  check(!key('A'),"raw Windows modifier rejected");key('A',RI_KEY_BREAK);
  key(VK_LWIN,RI_KEY_BREAK);key(VK_MENU,RI_KEY_E0|RI_KEY_BREAK);
  key(VK_SHIFT,0,0x2a);key(VK_MENU);
  check(!key('A'),"raw left Shift is tracked");key('A',RI_KEY_BREAK);
  key(VK_SHIFT,RI_KEY_BREAK,0x2a);
  check(key('A'),"raw Shift release restores Alt+A");
  RAWKEYBOARD invalid{};invalid.VKey=255;
  check(!keys.update(invalid,MOD_ALT,'A'),"raw fake key rejected");
 }
 {
  // 3x2 的未旋转纹理带有行填充，目标副屏位于主屏左上方。
  BYTE pixels[32]{};pixels[0]=1;pixels[4]=2;pixels[8]=3;
  pixels[16]=4;pixels[20]=5;pixels[24]=6;
  const DXGI_MODE_ROTATION rotations[]={DXGI_MODE_ROTATION_IDENTITY,
   DXGI_MODE_ROTATION_ROTATE90,DXGI_MODE_ROTATION_ROTATE180,DXGI_MODE_ROTATION_ROTATE270};
  const BYTE expected[4][6]={{1,2,3,4,5,6},{4,1,5,2,6,3},{6,5,4,3,2,1},{3,6,2,5,1,4}};
  for(int r=0;r<4;++r){
   int width=r%2?2:3,height=r%2?3:2;
   RECT output{-4,-3,-4+width,-3+height};
   RECT desktop{-5,-4,1,1};
   std::vector<BYTE> copied(6*5*4,0);
   check(copy_duplication_pixels(pixels,3,2,16,rotations[r],output,copied.data(),desktop),
         "DXGI rotated padded frame copied");
   bool equal=true;
   for(int y=0;y<height;++y)for(int x=0;x<width;++x)
    equal=equal&&copied[((y+1)*6+x+1)*4]==expected[r][y*width+x];
   check(equal&&copied[0]==0,"DXGI rotation and negative desktop coordinates");
  }
  RECT output{0,0,3,2},cropped{1,0,3,2};BYTE copied[16]{};
  check(copy_duplication_pixels(pixels,3,2,16,rotations[0],output,copied,cropped)&&
        copied[0]==2&&copied[4]==3&&copied[8]==5&&copied[12]==6,
        "DXGI clips output to desktop");
  check(!copy_duplication_pixels(pixels,3,2,8,rotations[0],output,copied,cropped),
        "DXGI rejects short row pitch");
 }
 {
  LogTrace trace;
  auto test=[&](bool ok,const char* name){
   logf(L"多屏等待预算回归：%s 成功=%d",u8_to_w(name).c_str(),(int)ok);check(ok,name);
  };
  uint64_t clock=0;int pauses=0;
  auto now=[&](){return clock;};
  auto pause=[&](unsigned ms){clock+=ms;++pauses;};
  auto all=wait_for_desktop_frames(3,[](size_t){return true;},now,pause);
  test(all==3&&clock==0&&pauses==0,"ready desktop frames return immediately without waiting");
  int attempts[3]{};uint64_t finished[3]{};
  auto delayed=[&](size_t i){
   ++attempts[i];
   if(i&&clock>=i*8){finished[i]=clock;return true;}
   return false;
  };
  all=wait_for_desktop_frames(3,delayed,now,pause);
  test(all==2&&finished[1]==8&&finished[2]==16,
       "idle first output never blocks ready second and third screens");
  test(clock==300&&attempts[1]==3&&attempts[2]==5,
       "all monitors share one 300ms budget and finished outputs stop polling");
  clock=0;pauses=0;
  all=wait_for_desktop_frames(6,[](size_t){return false;},now,pause);
  test(all==0&&clock==300,"six timed-out screens do not accumulate six separate waits");
  clock=0;pauses=0;int notices=0;
  all=wait_for_desktop_frames(1,[&](size_t){++notices;return clock>=20;},now,pause);
  test(all==1&&clock==20&&notices>1,"pointer-only notifications keep polling until desktop image is ready");
  clock=0;pauses=0;
  all=wait_for_desktop_frames(2,[&](size_t i){return i==0||clock>=12;},now,pause);
  test(all==2&&clock==12,"failed output can finish while another output still waits for image");
  clock=0;pauses=0;
  all=wait_for_desktop_frames(0,[](size_t){return false;},now,pause);
  test(all==0&&clock==0&&pauses==0,"no outputs require no wait or background polling");
 }
 Gdiplus::Bitmap bmp(240,180,PixelFormat32bppRGB);std::mt19937 rnd(42);
 for(int y=0;y<180;++y)for(int x=0;x<240;++x)bmp.SetPixel(x,y,Gdiplus::Color(255,(BYTE)rnd(),(BYTE)rnd(),(BYTE)rnd()));
 check(save_bitmap_file(&bmp,dir+L"\\x.png",92),"encode PNG");check(head(dir+L"\\x.png",4)==std::string("\x89PNG",4),"PNG signature");
 check(save_bitmap_file(&bmp,dir+L"\\x.bmp",92),"encode BMP");check(head(dir+L"\\x.bmp",2)=="BM","BMP signature");
 check(save_bitmap_file(&bmp,dir+L"\\low.jpg",10)&&save_bitmap_file(&bmp,dir+L"\\high.jpg",95),"encode JPEG quality variants");
 check(head(dir+L"\\low.jpg",2)==std::string("\xff\xd8",2),"JPEG signature");
 check(std::filesystem::file_size(dir+L"\\low.jpg")<std::filesystem::file_size(dir+L"\\high.jpg"),"JPEG quality changes output size");
 check(!save_bitmap_file(&bmp,dir+L"\\bad.xyz",92),"reject unsupported extension");
 {
  Gdiplus::Bitmap odd(239,65,PixelFormat32bppARGB);
  {Gdiplus::Graphics graphics(&odd);graphics.Clear(Gdiplus::Color(128,255,0,255));}
  bool encoded=save_bitmap_file(&odd,dir+L"\\alpha.png",92);
  Gdiplus::Bitmap alpha((dir+L"\\alpha.png").c_str());Gdiplus::Color pixel;
  auto status=alpha.GetPixel(238,64,&pixel);
  logf(L"PNG 奇数宽度与透明度回归：编码成功=%d 读取状态=%d ARGB=(%u,%u,%u,%u)",
       (int)encoded,(int)status,pixel.GetA(),pixel.GetR(),pixel.GetG(),pixel.GetB());
  check(encoded&&status==Gdiplus::Ok&&pixel.GetA()==128&&
        pixel.GetR()==255&&pixel.GetG()==0&&pixel.GetB()==255,"PNG preserves odd-width pixels and transparency");
  {Gdiplus::Graphics graphics(&odd);graphics.Clear(Gdiplus::Color(255,32,64,96));}
  encoded=save_bitmap_file(&odd,dir+L"\\odd.bmp",92);
  Gdiplus::Bitmap oddBmp((dir+L"\\odd.bmp").c_str());
  check(encoded&&oddBmp.GetWidth()==239&&oddBmp.GetHeight()==65&&
        oddBmp.GetPixel(238,64,&pixel)==Gdiplus::Ok&&pixel.GetR()==32&&pixel.GetG()==64&&pixel.GetB()==96,
        "BMP preserves odd-width row stride and color");
 }
 {
  s.ocr_enabled=false;s.translate_enabled=true;s.ocr_engine=L"ppocr";save_settings(s);
  std::atomic<bool> cancel{true};std::wstring result,error;
  check(!ocr_recognize(&bmp,result,error,&cancel)&&error.find(L"主开关已关闭")!=std::wstring::npos,
        "OCR master switch blocks model loading even for image translation");
 }
 std::wstring direct;check(save_bitmap_direct(&bmp,dir,L"png",92,direct)&&std::filesystem::exists(direct),"direct save to configured directory");
 Gdiplus::Color unused;bmp.GetPixel(-1,-1,&unused);
 check(save_bitmap_file(&bmp,dir+L"\\after-status-error.png",92),"historical bitmap error does not prevent valid image encoding");
 const int w=160,h=180,shift=31;std::vector<BYTE> source(w*(h+shift)*4);for(auto&b:source)b=(BYTE)rnd();
 std::vector<BYTE>a(source.begin(),source.begin()+w*h*4),b(source.begin()+w*shift*4,source.end());
 check(longshot_overlap(a,h,b,h,w)==h-shift,"odd-pixel scrolling overlap");
 check(longshot_overlap(a,h,a,h,w)==h,"unchanged frame detected");
 for(auto&v:b)v=(BYTE)rnd();check(longshot_overlap(a,h,b,h,w)==0,"reject unrelated frames");
 std::fill(a.begin(),a.end(),255);std::fill(b.begin(),b.end(),255);check(longshot_overlap(a,h,b,h,w)==0,"reject blank ambiguous frames");
 for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<4;++c)a[(y*w+x)*4+c]=b[(y*w+x)*4+c]=(BYTE)(((y%12)*31+x*7)%256);
 check(longshot_overlap(a,h,b,h,w)==0,"reject repeating pattern ambiguity");
 {
  const int width=32,frameHeight=80,scroll=17,steps=1200;
  const int finalHeight=frameHeight+scroll*steps;
  std::vector<BYTE> document((size_t)width*finalHeight*4);
  for(auto& value:document)value=(BYTE)rnd();
  std::vector<BYTE> frame(document.begin(),document.begin()+width*frameHeight*4);
  LongshotStitcher stitch(width,frameHeight,frame);
  bool added=true;
  for(int step=1;step<=steps;++step){
   auto begin=document.begin()+(size_t)width*scroll*step*4;
   frame.assign(begin,begin+width*frameHeight*4);
   if(stitch.append(frame,frameHeight)!=LongshotAppend::Added){added=false;break;}
  }
  check(added&&stitch.height==finalHeight&&stitch.pixels==document,
        "stitch 1200 scrolls beyond 20000 pixels without truncation or lost rows");
  check(stitch.append(frame,frameHeight)==LongshotAppend::Unchanged&&stitch.pixels==document,
        "page bottom does not append duplicate rows");
  for(auto& value:frame)value=(BYTE)rnd();
  check(stitch.append(frame,frameHeight)==LongshotAppend::Unmatched&&stitch.pixels==document,
        "unmatched frame preserves all accumulated content");
  frame.assign(document.begin(),document.begin()+width*frameHeight*4);
  LongshotStitcher limited(width,frameHeight,frame,(size_t)width*(frameHeight+10)*4);
  frame.assign(document.begin()+width*scroll*4,document.begin()+width*(scroll+frameHeight)*4);
  check(limited.append(frame,frameHeight)==LongshotAppend::MemoryLimit&&limited.height==frameHeight+10,
        "memory budget has an explicit finish result and keeps captured rows");
  check(std::equal(limited.pixels.begin(),limited.pixels.end(),document.begin()),
        "memory-limited result remains a continuous image");
  frame.assign(document.begin(),document.begin()+width*frameHeight*4);
  LongshotStitcher tinyScroll(width,frameHeight,frame);
  frame.assign(document.begin()+width*2*4,document.begin()+width*(frameHeight+2)*4);
  check(tinyScroll.append(frame,frameHeight)==LongshotAppend::Added&&tinyScroll.height==frameHeight+2,
        "final two-pixel scroll is retained instead of mistaken for page bottom");
  document.resize((size_t)width*70080*4);
  for(auto& value:document)value=(BYTE)rnd();
  frame.assign(document.begin(),document.begin()+width*frameHeight*4);
  LongshotStitcher encoderLimited(width,frameHeight,frame);
  LongshotAppend result=LongshotAppend::Added;
  for(int offset=scroll;offset+frameHeight<=70080;offset+=scroll){
   frame.assign(document.begin()+(size_t)width*offset*4,document.begin()+(size_t)width*(offset+frameHeight)*4);
   result=encoderLimited.append(frame,frameHeight);
   if(result!=LongshotAppend::Added)break;
  }
  check(result==LongshotAppend::EncoderLimit&&encoderLimited.height==LONGSHOT_MAX_ENCODED_HEIGHT&&
        std::equal(encoderLimited.pixels.begin(),encoderLimited.pixels.end(),document.begin()),
        "encoder boundary preserves a continuous image within savable dimensions");
  Gdiplus::Bitmap veryTall(64,LONGSHOT_MAX_ENCODED_HEIGHT,PixelFormat32bppRGB);
  {Gdiplus::Graphics graphics(&veryTall);graphics.Clear(Gdiplus::Color::White);}
  check(save_bitmap_direct(&veryTall,dir,L"png",92,direct),"save PNG at the supported encoder height boundary");
  Gdiplus::Bitmap readBack(direct.c_str());Gdiplus::Color lastPixel;
  check(readBack.GetWidth()==64&&readBack.GetHeight()==LONGSHOT_MAX_ENCODED_HEIGHT&&
        readBack.GetPixel(0,LONGSHOT_MAX_ENCODED_HEIGHT-1,&lastPixel)==Gdiplus::Ok&&lastPixel.GetValue()==0xffffffff,
        "boundary-height PNG preserves full dimensions and final row");
  check(save_bitmap_direct(&veryTall,dir,L"bmp",92,direct),"save BMP at the supported encoder height boundary");
 }
 if(argc>1&&std::wstring(argv[1])==L"--ocr"){
   s.ocr_enabled=true;s.ocr_engine=L"hunyuan";s.ocr_model_dir=std::wstring(_wgetenv(L"LOCALAPPDATA"))+L"\\weijie\\hunyuan-ocr";save_settings(s);
  hunyuan_download_models_async();while(hunyuan_download_state().busy)Sleep(100);hunyuan_download_shutdown();
  check(hunyuan_model_ready(),"model hash validation and readiness");
  Gdiplus::Bitmap image(800,260,PixelFormat32bppRGB);{Gdiplus::Graphics g(&image);g.Clear(Gdiplus::Color::White);Gdiplus::Font font(L"Microsoft YaHei",30);Gdiplus::SolidBrush ink(Gdiplus::Color::Black);g.DrawString(L"Hello 123\n混元 OCR 测试",-1,&font,Gdiplus::PointF(20,20),&ink);}
  std::wstring out,err;bool ok=ocr_recognize(&image,out,err);check(ok,"real Hunyuan inference");check(out.find(L"Hello 123")!=std::wstring::npos&&out.find(L"混元")!=std::wstring::npos,"bilingual OCR output");
  std::atomic<bool> cancelled{true};check(!ocr_recognize(&image,out,err,&cancelled),"cancel controlled OCR child");
 }
 if(argc>1&&(std::wstring(argv[1])==L"--fast"||std::wstring(argv[1])==L"--fast-gpu")){
  auto diag=dir+L"\\ocr-diagnostics.jsonl";
  SetEnvironmentVariableW(L"JJ_OCR_DIAGNOSTICS",diag.c_str());
  s.ocr_enabled=true;s.ocr_engine=L"ppocr";
  s.ocr_backend=std::wstring(argv[1])==L"--fast-gpu"?L"gpu":L"cpu";save_settings(s);
   check(fast_ocr_available(),"PP-OCRv6 engine available");
  Gdiplus::Bitmap image(800,260,PixelFormat32bppRGB);{Gdiplus::Graphics g(&image);g.Clear(Gdiplus::Color::White);Gdiplus::Font font(L"Microsoft YaHei",30);Gdiplus::SolidBrush ink(Gdiplus::Color::Black);g.DrawString(L"Hello 123\nfast OCR test",-1,&font,Gdiplus::PointF(20,20),&ink);}
  std::wstring out,err;bool fastOk=false;
  std::thread worker([&]{fastOk=fast_ocr_recognize(&image,out,err);});
  worker.join();
  if(!fastOk){std::wcout<<L"FAST_ERR="<<err<<L" len="<<err.size()<<L" codes=";for(wchar_t c:err)std::wcout<<std::hex<<(int)c<<L",";std::wcout<<std::dec<<std::endl;}check(fastOk,"PP-OCRv6 inference");check(out.find(L"Hello")!=std::wstring::npos,"PP-OCRv6 output");
  check(fast_ocr_recognize(&image,out,err),"repeat OCR request");
  DWORD resident=own_ocr_pid();HANDLE model=OpenProcess(SYNCHRONIZE,FALSE,resident);
  check(resident!=0&&model!=nullptr,"real model process is resident after recognition");
  s.ocr_enabled=false;check(save_settings(s),"disable master switch after real inference");
  check(WaitForSingleObject(model,0)==WAIT_OBJECT_0,"real CPU or Vulkan model process exits when disabled");
  check(own_ocr_pid()==0,"disabled real backend has no resident model process");
  std::ifstream diagnostics{std::filesystem::path(diag)};
  std::string line;int ready=0,requests=0,reused=0;
  while(std::getline(diagnostics,line)){
   if(line.find("\"event\": \"worker_ready\"")!=std::string::npos)++ready;
   if(line.find("\"event\": \"request_end\"")!=std::string::npos){
    ++requests;if(line.find("\"reused\": true")!=std::string::npos)++reused;
   }
  }
  check(ready==1&&requests==2&&reused==1,"reuse one sidecar and compiled engine");
  s.ocr_enabled=true;save_settings(s);
  check(own_ocr_pid()==0,"re-enable real backend without preloading");
  check(fast_ocr_recognize(&image,out,err)&&own_ocr_pid()!=resident,"real model reloads on next request");
  CloseHandle(model);
  resident=own_ocr_pid();model=OpenProcess(SYNCHRONIZE,FALSE,resident);
  s.ocr_idle_minutes=1;save_settings(s);
  check(!ocr_reclaim_idle(GetTickCount64()+59000),"real model stays loaded before idle deadline");
  check(ocr_reclaim_idle(GetTickCount64()+61000)&&WaitForSingleObject(model,0)==WAIT_OBJECT_0&&
        own_ocr_pid()==0&&ocr_is_enabled(),"idle reclaim releases real CPU or Vulkan model without disabling OCR");
  CloseHandle(model);
  check(fast_ocr_recognize(&image,out,err)&&own_ocr_pid()!=resident,"real model reloads after automatic idle reclaim");
  s.ocr_enabled=false;save_settings(s);
  check(own_ocr_pid()==0,"reloaded real model is released again");
  s.ocr_enabled=true;save_settings(s);
  std::atomic<bool> cancelled{true};
  check(!fast_ocr_recognize(&image,out,err,&cancelled)&&err==L"已取消识别"&&own_ocr_pid()==0,
        "cancel enabled OCR before launching worker");
  s.ocr_enabled=false;save_settings(s);
 }
 std::cout<<"TOTAL "<<count<<" passed\nArtifacts: ";std::wcout<<dir<<std::endl;
 // Bitmap lifetime ends before the process exits. Avoid shutting GDI+ down beneath stack objects.
 return 0;
}
