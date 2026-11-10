#include "../src/settings.h"
#include "../src/output.h"
#include "../src/longshot.h"
#include "../src/ocr.h"
#include "../src/fast_ocr.h"
#include "../src/ocr_setup.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
static int count=0;
void check(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;if(!ok)exit(1);++count;}
std::string head(const std::wstring&p,int n){std::ifstream f(std::filesystem::path(p),std::ios::binary);std::string s(n,'\0');f.read(s.data(),n);return s;}
int wmain(int argc,wchar_t**argv){
 wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);auto dir=std::wstring(temp)+L"weijie-core-tests-"+std::to_wstring(GetCurrentProcessId());std::filesystem::create_directories(dir);
 // Isolated settings: never overwrite user configuration during regression tests.
 SetEnvironmentVariableW(L"APPDATA",dir.c_str());
 Gdiplus::GdiplusStartupInput input;ULONG_PTR token;Gdiplus::GdiplusStartup(&token,&input,nullptr);
 Settings s;s.hotkey=L"Ctrl+Alt+K";s.hotkey_enabled=false;s.save_dir=L"C:\\测试目录\\Screens";s.image_format=L"bmp";s.jpeg_quality=77;s.ocr_enabled=false;s.translate_enabled=false;s.stroke_width=8;s.text_size=32;
 check(save_settings(s),"atomic settings write");auto loaded=load_settings();
 check(loaded.save_dir==s.save_dir,"Unicode path round trip");check(!loaded.hotkey_enabled,"monitor switch round trip");check(!loaded.ocr_enabled,"OCR opt-out persists");check(!loaded.translate_enabled,"translation opt-out persists");
 check(loaded.stroke_width==8&&loaded.text_size==32,"annotation settings persist");
 check(loaded.jpeg_quality==77&&loaded.image_format==L"bmp","encoding settings persist");
 UINT mods,vk;check(resolve_hotkey(L"Ctrl+Alt+K",mods,vk)&&mods==(MOD_CONTROL|MOD_ALT)&&vk=='K',"custom hotkey");
 check(resolve_hotkey(L"F24",mods,vk)&&vk==VK_F24,"F24 hotkey");check(resolve_hotkey(L"PrintScreen",mods,vk)&&vk==VK_SNAPSHOT,"PrintScreen hotkey");
 check(!resolve_hotkey(L"A",mods,vk),"reject plain letter");check(!resolve_hotkey(L"Ctrl+F25",mods,vk),"reject invalid F key");check(!resolve_hotkey(L"Ctrl+Ctrl+A",mods,vk),"reject duplicate modifier");
 Gdiplus::Bitmap bmp(240,180,PixelFormat32bppRGB);std::mt19937 rnd(42);
 for(int y=0;y<180;++y)for(int x=0;x<240;++x)bmp.SetPixel(x,y,Gdiplus::Color(255,(BYTE)rnd(),(BYTE)rnd(),(BYTE)rnd()));
 check(save_bitmap_file(&bmp,dir+L"\\x.png",92),"encode PNG");check(head(dir+L"\\x.png",4)==std::string("\x89PNG",4),"PNG signature");
 check(save_bitmap_file(&bmp,dir+L"\\x.bmp",92),"encode BMP");check(head(dir+L"\\x.bmp",2)=="BM","BMP signature");
 check(save_bitmap_file(&bmp,dir+L"\\low.jpg",10)&&save_bitmap_file(&bmp,dir+L"\\high.jpg",95),"encode JPEG quality variants");
 check(head(dir+L"\\low.jpg",2)==std::string("\xff\xd8",2),"JPEG signature");
 check(std::filesystem::file_size(dir+L"\\low.jpg")<std::filesystem::file_size(dir+L"\\high.jpg"),"JPEG quality changes output size");
 check(!save_bitmap_file(&bmp,dir+L"\\bad.xyz",92),"reject unsupported extension");
 std::wstring direct;check(save_bitmap_direct(&bmp,dir,L"png",92,direct)&&std::filesystem::exists(direct),"direct save to configured directory");
 const int w=160,h=180,shift=31;std::vector<BYTE> source(w*(h+shift)*4);for(auto&b:source)b=(BYTE)rnd();
 std::vector<BYTE>a(source.begin(),source.begin()+w*h*4),b(source.begin()+w*shift*4,source.end());
 check(longshot_overlap(a,h,b,h,w)==h-shift,"odd-pixel scrolling overlap");
 check(longshot_overlap(a,h,a,h,w)==h,"unchanged frame detected");
 for(auto&v:b)v=(BYTE)rnd();check(longshot_overlap(a,h,b,h,w)==0,"reject unrelated frames");
 std::fill(a.begin(),a.end(),255);std::fill(b.begin(),b.end(),255);check(longshot_overlap(a,h,b,h,w)==0,"reject blank ambiguous frames");
 for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<4;++c)a[(y*w+x)*4+c]=b[(y*w+x)*4+c]=(BYTE)(((y%12)*31+x*7)%256);
 check(longshot_overlap(a,h,b,h,w)==0,"reject repeating pattern ambiguity");
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
  fast_ocr_shutdown();
  std::ifstream diagnostics{std::filesystem::path(diag)};
  std::string line;int ready=0,requests=0,reused=0;
  while(std::getline(diagnostics,line)){
   if(line.find("\"event\": \"worker_ready\"")!=std::string::npos)++ready;
   if(line.find("\"event\": \"request_end\"")!=std::string::npos){
    ++requests;if(line.find("\"reused\": true")!=std::string::npos)++reused;
   }
  }
  check(ready==1&&requests==2&&reused==1,"reuse one sidecar and compiled engine");
  std::atomic<bool> cancelled{true};
  check(!fast_ocr_recognize(&image,out,err,&cancelled),"cancel OCR before launching worker");
 }
 std::cout<<"TOTAL "<<count<<" passed\nArtifacts: ";std::wcout<<dir<<std::endl;
 // Bitmap lifetime ends before the process exits. Avoid shutting GDI+ down beneath stack objects.
 return 0;
}
