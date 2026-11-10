#include "style_popup.h"
#include "overlay.h"
#include <commctrl.h>
#include <cwctype>
#pragma comment(lib,"comctl32.lib")
using namespace Gdiplus;
Color hsv_color(double h,double s,double v,BYTE a){
 h=fmod(h+360.0,360.0);s=std::clamp(s,0.0,1.0);v=std::clamp(v,0.0,1.0);
 double c=v*s,x=c*(1-abs(fmod(h/60,2.0)-1)),m=v-c,r=0,g=0,b=0;
 if(h<60){r=c;g=x;}else if(h<120){r=x;g=c;}else if(h<180){g=c;b=x;}else if(h<240){g=x;b=c;}else if(h<300){r=x;b=c;}else{r=c;b=x;}
 return Color(a,(BYTE)lround((r+m)*255),(BYTE)lround((g+m)*255),(BYTE)lround((b+m)*255));
}
void color_hsv(Color c,double&h,double&s,double&v){
 double r=c.GetR()/255.0,g=c.GetG()/255.0,b=c.GetB()/255.0,lo=std::min({r,g,b}),hi=std::max({r,g,b}),d=hi-lo;
 v=hi;s=hi?d/hi:0;if(d==0){h=0;return;}
 h=hi==r?60*fmod((g-b)/d,6.0):hi==g?60*((b-r)/d+2):60*((r-g)/d+4);if(h<0)h+=360;
}
std::vector<DWORD> load_recent_colors(const std::wstring& text){
 std::vector<DWORD> out;size_t pos=0;
 while(pos<text.size()&&out.size()<12){auto token=text.substr(pos,8);if(token.size()!=8||!std::all_of(token.begin(),token.end(),[](wchar_t c){return iswxdigit(c)!=0;}))break;
  out.push_back((DWORD)std::stoull(token,nullptr,16));pos+=8;if(pos<text.size()){if(text[pos]!=L',')break;++pos;}}
 return out;
}
std::wstring encode_recent_colors(const std::vector<DWORD>& colors){std::wstring out;for(size_t i=0;i<std::min<size_t>(12,colors.size());++i){wchar_t b[16];swprintf_s(b,L"%08X",colors[i]);if(i)out+=L",";out+=b;}return out;}
#include "style_popup_body.inc"
