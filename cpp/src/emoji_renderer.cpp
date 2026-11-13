#include "emoji_renderer.h"
#include <filesystem>
#include <cwctype>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib,"windowscodecs.lib")

using namespace Gdiplus;
using Microsoft::WRL::ComPtr;

size_t StampAnimation::frame_index(uint64_t time) const{
    if(!duration||frames.size()<2)return 0;
    time%=duration;
    for(size_t i=0;i<delays.size();++i){
        if(time<delays[i])return i;
        time-=delays[i];
    }
    return 0;
}
uint64_t StampAnimation::next_change(uint64_t time) const{
    if(!duration||frames.size()<2)return UINT64_MAX;
    uint64_t elapsed=time%duration,edge=0;
    for(auto delay:delays){edge+=delay;if(edge>elapsed)return time+edge-elapsed;}
    return time+duration-elapsed;
}

namespace {
struct StampImage {
    std::wstring name;
    std::filesystem::path path;
    std::shared_ptr<Bitmap> bitmap;
    std::shared_ptr<Bitmap> full;
    std::shared_ptr<StampAnimation> animation;
    bool animationLoaded=false;
    std::wstring group;
};
std::vector<StampImage> g_images;
std::vector<StampGroup> g_imageGroups;
bool g_imagesLoaded=false;

UINT gif_metadata(IWICMetadataQueryReader* reader,const wchar_t* key,UINT fallback=0){
    PROPVARIANT value{};UINT result=fallback;
    if(reader&&SUCCEEDED(reader->GetMetadataByName(key,&value))){
        if(value.vt==VT_UI1)result=value.bVal;
        else if(value.vt==VT_UI2)result=value.uiVal;
        else if(value.vt==VT_UI4)result=value.ulVal;
        else if(value.vt==VT_BOOL)result=value.boolVal!=VARIANT_FALSE;
    }
    PropVariantClear(&value);return result;
}

std::shared_ptr<StampAnimation> decode_stamp_animation(const std::filesystem::path& path){
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
    UINT count=0;if(SUCCEEDED(hr))hr=decoder->GetFrameCount(&count);
    if(FAILED(hr)){
        logf(L"GIF 动画加载失败：文件=%s 解码器 HRESULT=%08lx，尝试保留静态首帧",path.filename().c_str(),hr);
        return nullptr;
    }
    if(count<2)return nullptr;
    ComPtr<IWICMetadataQueryReader> metadata;decoder->GetMetadataQueryReader(&metadata);
    UINT width=gif_metadata(metadata.Get(),L"/logscrdesc/Width");
    UINT height=gif_metadata(metadata.Get(),L"/logscrdesc/Height");
    if(!width||!height){
        logf(L"GIF 动画加载失败：文件=%s 逻辑画布尺寸无效=%ux%u",path.filename().c_str(),width,height);return nullptr;
    }
    float scale=std::min(1.0f,std::max(256.0f,SF(256))/std::max(width,height));
    int w=std::max(1,(int)lround(width*scale)),h=std::max(1,(int)lround(height*scale));
    auto result=std::make_shared<StampAnimation>();
    Bitmap canvas(w,h,PixelFormat32bppPARGB);
    Color background(0,0,0,0);
    ComPtr<IWICPalette> palette;factory->CreatePalette(&palette);
    if(palette&&SUCCEEDED(decoder->CopyPalette(palette.Get()))){
        WICColor colors[256]{};UINT n=0;palette->GetColors(256,colors,&n);
        UINT index=gif_metadata(metadata.Get(),L"/logscrdesc/BackgroundColorIndex");
        if(index<n)background=Color(colors[index]);
    }
    {Graphics g(&canvas);g.Clear(Color(0,0,0,0));}
    for(UINT i=0;i<count;++i){
        ComPtr<IWICBitmapFrameDecode> frame;hr=decoder->GetFrame(i,&frame);
        ComPtr<IWICMetadataQueryReader> info;
        if(SUCCEEDED(hr))frame->GetMetadataQueryReader(&info);
        UINT fw=0,fh=0;if(SUCCEEDED(hr))hr=frame->GetSize(&fw,&fh);
        ComPtr<IWICFormatConverter> converter;
        if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
        if(SUCCEEDED(hr))hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
        // 帧先经 WIC 缩放，再在小画布中按偏移和处置方式合成。
        ComPtr<IWICBitmapScaler> scaler;
        UINT rw=std::max(1,(int)lround(fw*scale)),rh=std::max(1,(int)lround(fh*scale));
        if(SUCCEEDED(hr))hr=factory->CreateBitmapScaler(&scaler);
        if(SUCCEEDED(hr))hr=scaler->Initialize(converter.Get(),rw,rh,WICBitmapInterpolationModeFant);
        std::vector<BYTE> pixels((size_t)rw*rh*4);
        if(SUCCEEDED(hr))hr=scaler->CopyPixels(nullptr,rw*4,(UINT)pixels.size(),pixels.data());
        if(FAILED(hr)){
            logf(L"GIF 动画加载失败：文件=%s 帧=%u HRESULT=%08lx",path.filename().c_str(),i,hr);return nullptr;
        }
        UINT disposal=gif_metadata(info.Get(),L"/grctlext/Disposal");
        bool transparent=gif_metadata(info.Get(),L"/grctlext/TransparencyFlag")!=0;
        Rect area((int)lround(gif_metadata(info.Get(),L"/imgdesc/Left")*scale),
                  (int)lround(gif_metadata(info.Get(),L"/imgdesc/Top")*scale),rw,rh);
        std::unique_ptr<Bitmap> previous;
        if(disposal==3)previous.reset(canvas.Clone(0,0,w,h,PixelFormat32bppPARGB));
        {
            Graphics g(&canvas);
            if(i==0&&!transparent)g.Clear(background);
            Bitmap raw(rw,rh,rw*4,PixelFormat32bppPARGB,pixels.data());
            g.DrawImage(&raw,area,0,0,rw,rh,UnitPixel);
        }
        auto snapshot=std::shared_ptr<Bitmap>(canvas.Clone(0,0,w,h,PixelFormat32bppPARGB));
        if(!snapshot||snapshot->GetLastStatus()!=Ok)return nullptr;
        result->frames.push_back(snapshot);
        UINT delay=gif_metadata(info.Get(),L"/grctlext/Delay",10)*10;
        if(!delay)delay=100;
        result->delays.push_back(delay);result->duration+=delay;
        if(disposal==2){
            Graphics g(&canvas);g.SetCompositingMode(CompositingModeSourceCopy);
            SolidBrush clear(transparent?Color(0,0,0,0):background);g.FillRectangle(&clear,area);
        }else if(disposal==3&&previous){
            Graphics g(&canvas);g.SetCompositingMode(CompositingModeSourceCopy);
            g.DrawImage(previous.get(),0,0,w,h);
        }
    }
    logf(L"GIF 动画加载完成：文件=%s 帧=%zu 时长=%llums 缓存=%dx%d；已合成透明、偏移及恢复背景/上一帧处置",
        path.filename().c_str(),result->frames.size(),result->duration,w,h);
    return result;
}

std::shared_ptr<Bitmap> decode_stamp_image(const std::filesystem::path& path,float limit){
    std::unique_ptr<Bitmap> source(Bitmap::FromFile(path.c_str(),FALSE));
    Status status=source?source->GetLastStatus():OutOfMemory;
    if(status!=Ok||!source->GetWidth()||!source->GetHeight()){
        logf(L"图片加载跳过：文件=%s 解码状态=%d；其他图片继续显示",path.filename().c_str(),(int)status);
        return nullptr;
    }
    UINT width=source->GetWidth(),height=source->GetHeight();
    float ratio=std::min(1.0f,limit/std::max(width,height));
    int w=std::max(1,(int)lround(width*ratio)),h=std::max(1,(int)lround(height*ratio));
    auto image=std::make_shared<Bitmap>(w,h,PixelFormat32bppPARGB);
    {
        Graphics g(image.get());g.Clear(Color(0,0,0,0));
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        ImageAttributes attributes;attributes.SetWrapMode(WrapModeTileFlipXY);
        status=g.DrawImage(source.get(),Rect(0,0,w,h),0,0,width,height,UnitPixel,&attributes);
    }
    if(image->GetLastStatus()!=Ok||status!=Ok){
        logf(L"图片加载跳过：文件=%s 缩放缓存失败 状态=%d",path.filename().c_str(),(int)status);
        return nullptr;
    }
    logf(L"图片加载完成：文件=%s 原始=%ux%u 缓存=%dx%d 上限=%.0fpx；源文件已释放，落地按软件设置缩放",
        path.filename().c_str(),width,height,w,h,limit);
    return image;
}
}

std::wstring emoji_directory(){
    return (std::filesystem::path(exe_path()).parent_path()/L"images").wstring();
}

int emoji_reload(){
    const auto directory=emoji_directory();
    logf(L"图片目录扫描请求：目录=%s，子目录作为分组，根目录图片归入未分类，动画按需加载",directory.c_str());
    std::vector<std::filesystem::path> paths;
    std::vector<std::wstring> folders;
    std::error_code error;
    std::filesystem::recursive_directory_iterator files(directory,
        std::filesystem::directory_options::skip_permission_denied,error),end;
    if(error){
        logf(L"图片目录扫描未完成：目录不可用 error=%d；选择窗口显示空目录提示",error.value());
    }else{
        for(;files!=end;files.increment(error)){
            if(error)break;
            if(files->is_directory(error)){
                folders.push_back(files->path().lexically_relative(directory).generic_wstring());
                continue;
            }
            if(!files->is_regular_file(error)){error.clear();continue;}
            std::wstring extension=files->path().extension().wstring();
            std::transform(extension.begin(),extension.end(),extension.begin(),towlower);
            if(extension==L".jpg"||extension==L".jpeg"||extension==L".png"||extension==L".gif"||
               extension==L".bmp"||extension==L".tif"||extension==L".tiff")
                paths.push_back(files->path());
        }
        if(error)logf(L"图片目录枚举中断：error=%d，保留已发现文件",error.value());
    }
    std::sort(paths.begin(),paths.end(),[](const auto& a,const auto& b){
        if(a.parent_path()!=b.parent_path())return _wcsicmp(a.parent_path().c_str(),b.parent_path().c_str())<0;
        return _wcsicmp(a.filename().c_str(),b.filename().c_str())<0;
    });
    std::vector<StampImage> next;
    std::vector<StampGroup> groups;
    auto add_group=[&](const std::wstring& key)->StampGroup&{
        for(auto& group:groups)if(group.key==key)return group;
        std::wstring name=key.empty()?L"未分类":!_wcsicmp(key.c_str(),L"default")?L"默认":key;
        groups.push_back({key,name,{}});return groups.back();
    };
    for(const auto& folder:folders)add_group(folder);
    for(const auto& path:paths){
        auto image=decode_stamp_image(path,std::max(26.0f,SF(26)));
        if(image){
            auto key=path.parent_path().lexically_relative(directory).generic_wstring();
            if(key==L".")key.clear();
            add_group(key).images.push_back((int)next.size());
            StampImage entry;entry.name=path.filename().wstring();entry.path=path;
            entry.bitmap=image;entry.group=key;next.push_back(std::move(entry));
        }
    }
    std::sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){
        auto rank=[](const auto& key){return !_wcsicmp(key.c_str(),L"default")?0:key.empty()?2:1;};
        int left=rank(a.key),right=rank(b.key);
        return left!=right?left<right:_wcsicmp(a.key.c_str(),b.key.c_str())<0;
    });
    for(const auto& group:groups)
        logf(L"图片分组已识别：目录=%s 显示=%s 图片=%zu",group.key.c_str(),group.name.c_str(),group.images.size());
    g_images=std::move(next);g_imageGroups=std::move(groups);g_imagesLoaded=true;
    logf(L"图片目录扫描返回：候选=%zu 可用=%zu 分组=%zu；目录删除不会补回，已放置图片保留独立引用",
        paths.size(),g_images.size(),g_imageGroups.size());
    return (int)g_images.size();
}

const std::vector<StampGroup>& emoji_groups(){
    if(!g_imagesLoaded)emoji_reload();
    return g_imageGroups;
}

int emoji_count(){
    if(!g_imagesLoaded)emoji_reload();
    return (int)g_images.size();
}

const wchar_t* emoji_text(int index){
    return index>=0&&index<emoji_count()?g_images[index].name.c_str():nullptr;
}

std::shared_ptr<Bitmap> emoji_image(int index){
    if(index<0||index>=emoji_count())return nullptr;
    auto& entry=g_images[index];
    if(!entry.full){
        auto animation=emoji_animation(index);
        entry.full=animation?animation->frames.front():decode_stamp_image(entry.path,std::max(256.0f,SF(256)));
    }
    return entry.full;
}

std::shared_ptr<StampAnimation> emoji_animation(int index){
    if(index<0||index>=emoji_count())return nullptr;
    auto& entry=g_images[index];
    if(!entry.animationLoaded){
        entry.animationLoaded=true;
        if(!_wcsicmp(entry.path.extension().c_str(),L".gif"))entry.animation=decode_stamp_animation(entry.path);
    }
    return entry.animation;
}
Bitmap* emoji_bitmap(int index){
    return index>=0&&index<emoji_count()?g_images[index].bitmap.get():nullptr;
}

RectF stamp_image_rect(Bitmap* image,const PointF& center,float size){
    if(!image||!image->GetWidth()||!image->GetHeight())return {};
    float ratio=size/std::max(image->GetWidth(),image->GetHeight());
    float w=image->GetWidth()*ratio,h=image->GetHeight()*ratio;
    return {center.X-w/2,center.Y-h/2,w,h};
}

void draw_stamp_image(Graphics& g,Bitmap* image,const PointF& center,float size){
    if(!image)return;
    auto state=g.Save();
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    ImageAttributes attributes;attributes.SetWrapMode(WrapModeTileFlipXY);
    g.DrawImage(image,stamp_image_rect(image,center,size),0,0,
        (REAL)image->GetWidth(),(REAL)image->GetHeight(),UnitPixel,&attributes);
    g.Restore(state);
}

void draw_emoji(Graphics& g,int index,const PointF& center,float size){
    draw_stamp_image(g,emoji_image(index).get(),center,size);
}
