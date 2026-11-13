#include "animated_image.h"
#include "emoji_renderer.h"
#include <filesystem>
#include <fstream>
#include <cstring>

using namespace Gdiplus;

uint64_t AnimatedScreenshot::duration() const{
    uint64_t total=0;
    for(const auto& a:annotations)if(a.kind==AK_EMOJI&&a.emojiAnimation)
        total=std::max(total,a.emojiAnimation->duration);
    return total;
}
uint64_t AnimatedScreenshot::next_change(uint64_t time) const{
    uint64_t next=duration();
    for(const auto& a:annotations)if(a.kind==AK_EMOJI&&a.emojiAnimation)
        next=std::min(next,a.emojiAnimation->next_change(time));
    return next;
}
std::unique_ptr<Bitmap> AnimatedScreenshot::render(uint64_t time,const RECT* region) const{
    if(!background)return nullptr;
    uint64_t period=duration();if(period)time%=period;
    UiScaleScope dpi(scale);
    int x=region?region->left:0,y=region?region->top:0;
    int w=region?region->right-x:background->GetWidth(),h=region?region->bottom-y:background->GetHeight();
    std::unique_ptr<Bitmap> image(background->Clone(x,y,w,h,PixelFormat32bppRGB));
    if(!image||image->GetLastStatus()!=Ok)return nullptr;
    Graphics g(image.get());
    g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);g.TranslateTransform((REAL)-x,(REAL)-y);
    for(const auto& a:annotations)a.draw(g,mosaic.get(),time);
    return image;
}

namespace {
struct GifBlock {std::vector<BYTE> palette,data;BYTE size=0,interlace=0;bool transparent=false;};
bool bitmap_pixels(Bitmap* image,std::vector<BYTE>& pixels);

struct GifQuantizer {
    struct Bin{uint64_t count=0,r=0,g=0,b=0;};
    std::vector<Bin> histogram=std::vector<Bin>(32768);
    std::vector<DWORD> colors;
    std::vector<BYTE> lookup=std::vector<BYTE>(32768);
    void add(Bitmap* bitmap,uint64_t weight=1){
        std::vector<BYTE> pixels;if(!bitmap_pixels(bitmap,pixels))return;
        for(size_t i=0;i<pixels.size();i+=4){
            BYTE b=pixels[i],g=pixels[i+1],r=pixels[i+2];
            auto& bin=histogram[((r>>3)<<10)|((g>>3)<<5)|(b>>3)];
            bin.count+=weight;bin.r+=r*weight;bin.g+=g*weight;bin.b+=b*weight;
        }
    }
    void build(){
        std::vector<std::vector<int>> boxes(1);
        for(int i=0;i<32768;++i)if(histogram[i].count)boxes[0].push_back(i);
        auto range=[&](const std::vector<int>& box,int& axis){
            int low[3]={31,31,31},high[3]={0,0,0};
            for(int id:box)for(int c=0;c<3;++c){
                int value=(id>>(c*5))&31;low[c]=std::min(low[c],value);high[c]=std::max(high[c],value);
            }
            axis=0;for(int c=1;c<3;++c)if(high[c]-low[c]>high[axis]-low[axis])axis=c;
            return high[axis]-low[axis];
        };
        while(boxes.size()<255){
            int best=-1,axis=0;double score=-1;
            for(size_t i=0;i<boxes.size();++i)if(boxes[i].size()>1){
                int candidateAxis=0,span=range(boxes[i],candidateAxis);uint64_t count=0;
                for(int id:boxes[i])count+=histogram[id].count;
                double value=span*sqrt((double)count);
                if(value>score){score=value;best=(int)i;axis=candidateAxis;}
            }
            if(best<0)break;
            auto& box=boxes[best];uint64_t count=0,sum=0;
            for(int id:box)count+=histogram[id].count;
            std::sort(box.begin(),box.end(),[&](int a,int b){return ((a>>(axis*5))&31)<((b>>(axis*5))&31);});
            size_t split=0;
            while(split+1<box.size()&&sum<count/2)sum+=histogram[box[split++]].count;
            split=std::clamp(split,(size_t)1,box.size()-1);
            std::vector<int> tail(box.begin()+split,box.end());box.resize(split);boxes.push_back(std::move(tail));
        }
        for(const auto& box:boxes){
            Bin total;
            for(int id:box){auto& b=histogram[id];total.count+=b.count;total.r+=b.r;total.g+=b.g;total.b+=b.b;}
            colors.push_back(total.count?Color(255,(BYTE)(total.r/total.count),(BYTE)(total.g/total.count),
                (BYTE)(total.b/total.count)).GetValue():0xff000000);
        }
        while(colors.size()<255)colors.push_back(colors.back());
        colors.push_back(0xff000000);
        for(int id=0;id<32768;++id){
            int r=((id>>10)&31)*8+4,g=((id>>5)&31)*8+4,b=(id&31)*8+4,best=INT_MAX;
            for(int i=0;i<255;++i){
                Color color(colors[i]);int dr=r-color.GetR(),dg=g-color.GetG(),db=b-color.GetB();
                int distance=dr*dr+dg*dg+db*db;
                if(distance<best){best=distance;lookup[id]=(BYTE)i;}
            }
        }
    }
    std::unique_ptr<Bitmap> apply(Bitmap* source,const std::vector<BYTE>* changes=nullptr){
        std::vector<BYTE> pixels;if(!bitmap_pixels(source,pixels))return nullptr;
        auto indexed=std::make_unique<Bitmap>(source->GetWidth(),source->GetHeight(),PixelFormat8bppIndexed);
        std::vector<BYTE> storage(sizeof(ColorPalette)+255*sizeof(Gdiplus::ARGB));
        auto* palette=(ColorPalette*)storage.data();palette->Flags=0;palette->Count=256;
        memcpy(palette->Entries,colors.data(),256*sizeof(Gdiplus::ARGB));
        if(indexed->SetPalette(palette)!=Ok)return nullptr;
        Rect all(0,0,source->GetWidth(),source->GetHeight());BitmapData data{};
        if(indexed->LockBits(&all,ImageLockModeWrite,PixelFormat8bppIndexed,&data)!=Ok)return nullptr;
        for(int y=0;y<all.Height;++y)for(int x=0;x<all.Width;++x){
            auto* p=pixels.data()+((size_t)y*all.Width+x)*4;
            ((BYTE*)data.Scan0)[(ptrdiff_t)y*data.Stride+x]=changes&&!(*changes)[(size_t)y*all.Width+x]?
                255:lookup[((p[2]>>3)<<10)|((p[1]>>3)<<5)|(p[0]>>3)];
        }
        indexed->UnlockBits(&data);return indexed;
    }
};

bool encode_block(Bitmap* image,GifBlock& block){
    CLSID encoder{};UINT n=0,length=0;GetImageEncodersSize(&n,&length);
    std::vector<BYTE> codecs(length);
    auto* list=(ImageCodecInfo*)codecs.data();GetImageEncoders(n,length,list);
    bool found=false;
    for(UINT i=0;i<n;++i)if(!wcscmp(list[i].MimeType,L"image/gif")){encoder=list[i].Clsid;found=true;break;}
    if(!found)return false;
    IStream* stream=nullptr;
    if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)))return false;
    Status status=image->Save(stream,&encoder,nullptr);
    HGLOBAL buffer=nullptr;GetHGlobalFromStream(stream,&buffer);
    auto* data=(BYTE*)GlobalLock(buffer);size_t bytes=buffer?GlobalSize(buffer):0;
    bool ok=false;
    if(status==Ok&&data&&bytes>=13){
        size_t cursor=13;std::vector<BYTE> global;
        BYTE globalSize=data[10]&7;
        if(data[10]&0x80){
            size_t count=3ull*(2u<<globalSize);
            if(cursor+count<=bytes){global.assign(data+cursor,data+cursor+count);cursor+=count;}
            else cursor=bytes;
        }
        auto skip_blocks=[&](){
            while(cursor<bytes){size_t count=data[cursor++];if(!count)return true;
                if(count>bytes-cursor)return false;cursor+=count;}
            return false;
        };
        while(cursor<bytes){
            BYTE type=data[cursor++];
            if(type==0x21){if(cursor==bytes)break;++cursor;if(!skip_blocks())break;}
            else if(type==0x2c){
                if(cursor+9>bytes)break;
                BYTE packed=data[cursor+8];cursor+=9;
                block.size=packed&7;block.interlace=packed&0x40;
                if(packed&0x80){
                    size_t count=3ull*(2u<<block.size);if(count>bytes-cursor)break;
                    block.palette.assign(data+cursor,data+cursor+count);cursor+=count;
                }else{block.palette=global;block.size=globalSize;}
                size_t start=cursor;if(cursor==bytes)break;++cursor;
                if(skip_blocks()&&!block.palette.empty()){
                    block.data.assign(data+start,data+cursor);ok=true;
                }
                break;
            }else break;
        }
    }
    if(data)GlobalUnlock(buffer);stream->Release();return ok;
}

bool bitmap_pixels(Bitmap* image,std::vector<BYTE>& pixels){
    Rect all(0,0,image->GetWidth(),image->GetHeight());BitmapData data{};
    if(image->LockBits(&all,ImageLockModeRead,PixelFormat32bppRGB,&data)!=Ok)return false;
    pixels.resize((size_t)all.Width*all.Height*4);
    for(int y=0;y<all.Height;++y)memcpy(pixels.data()+(size_t)y*all.Width*4,
        (BYTE*)data.Scan0+(ptrdiff_t)y*data.Stride,(size_t)all.Width*4);
    image->UnlockBits(&data);return true;
}
}

bool save_animated_gif(const AnimatedScreenshot& scene,const std::wstring& path){
    LogTrace trace(scene.traceId);
    ULONGLONG started=GetTickCount64();
    wchar_t temporary[MAX_PATH]{};
    try{
    uint64_t duration=scene.duration();
    if(!scene.background||!duration){logf(L"动态 GIF 保存被拒绝：没有动态贴图");return false;}
    int width=scene.background->GetWidth(),height=scene.background->GetHeight();
    if(width>65535||height>65535){
        logf(L"动态 GIF 保存被拒绝：尺寸=%dx%d 超过 GIF 65535 像素边界",width,height);return false;
    }
    logf(L"动态 GIF 编码请求：文件=%s 尺寸=%dx%d 时长=%llums，短动画循环；静态背景仅编码一次",
        path.c_str(),width,height,duration);
    auto failed=[&](const wchar_t* stage){
        logf(L"动态 GIF 编码失败：阶段=%s 耗时=%llums，未替换目标文件，原截图保留",
            stage,GetTickCount64()-started);return false;
    };
    auto first=scene.render(0);if(!first)return failed(L"合成首帧");
    std::vector<BYTE> previous;if(!bitmap_pixels(first.get(),previous))return failed(L"读取首帧像素");
    // 动画以外的像素保持首帧，只重新合成动态贴图的并集。
    RECT active{width,height,0,0};
    {
        UiScaleScope dpi(scene.scale);
        for(const auto& a:scene.annotations)if(a.kind==AK_EMOJI&&a.emojiAnimation){
            auto r=a.bounds();
            active.left=std::min(active.left,(LONG)floor(r.X)-2);active.top=std::min(active.top,(LONG)floor(r.Y)-2);
            active.right=std::max(active.right,(LONG)ceil(r.GetRight())+2);
            active.bottom=std::max(active.bottom,(LONG)ceil(r.GetBottom())+2);
        }
    }
    active.left=std::clamp(active.left,0L,(LONG)width);active.top=std::clamp(active.top,0L,(LONG)height);
    active.right=std::clamp(active.right,active.left,(LONG)width);
    active.bottom=std::clamp(active.bottom,active.top,(LONG)height);
    std::vector<uint64_t> timeline{0};
    while(timeline.back()<duration){
        uint64_t next=scene.next_change(timeline.back());
        if(next<=timeline.back())return failed(L"动画帧时间边界无效");
        timeline.push_back(next);
    }
    logf(L"动态 GIF 调色板采样：时间段=%zu 变化区域=(%ld,%ld)-(%ld,%ld)，采样文字与动画叠加后的实际颜色",
        timeline.size()-1,active.left,active.top,active.right,active.bottom);
    GifQuantizer quantizer;quantizer.add(first.get(),timeline.size()-1);
    if(active.right>active.left&&active.bottom>active.top){
        uint64_t pixels=(uint64_t)(active.right-active.left)*(active.bottom-active.top);
        uint64_t weight=std::max<uint64_t>(1,(uint64_t)width*height/pixels);
        for(size_t i=1;i+1<timeline.size();++i){
            auto sample=scene.render(timeline[i],&active);
            if(!sample)return failed(L"采样动画与文字叠加后的颜色");
            quantizer.add(sample.get(),weight);
        }
    }
    quantizer.build();
    auto indexed=quantizer.apply(first.get());GifBlock pending;
    if(!indexed||!encode_block(indexed.get(),pending))return failed(L"首帧量化或 GIF 压缩");
    auto globalPalette=pending.palette;
    std::unique_ptr<Bitmap> corner(first->Clone(Rect(0,0,1,1),PixelFormat32bppRGB));
    auto cornerIndexed=corner?quantizer.apply(corner.get()):nullptr;GifBlock hold;
    if(!cornerIndexed||!encode_block(cornerIndexed.get(),hold))return failed(L"创建长停顿延时帧");
    BYTE holdColor[3]={previous[0],previous[1],previous[2]};
    auto folder=std::filesystem::path(path).parent_path().wstring();
    if(!GetTempFileNameW(folder.empty()?L".":folder.c_str(),L"wjj",0,temporary)){
        logf(L"动态 GIF 创建临时文件失败：error=%lu",GetLastError());return false;
    }
    std::ofstream file(std::filesystem::path(temporary),std::ios::binary|std::ios::trunc);
    if(!file){DeleteFileW(temporary);logf(L"动态 GIF 打开文件失败：%s",path.c_str());return false;}
    auto byte=[&](BYTE v){file.put((char)v);};
    auto word=[&](UINT v){byte((BYTE)v);byte((BYTE)(v>>8));};
    auto bytes=[&](const void* data,size_t count){file.write((const char*)data,(std::streamsize)count);};
    bytes("GIF89a",6);word(width);word(height);byte(0xf0|pending.size);byte(0);byte(0);
    bytes(pending.palette.data(),pending.palette.size());
    const BYTE loop[]={0x21,0xff,11,'N','E','T','S','C','A','P','E','2','.','0',3,1,0,0,0};
    bytes(loop,sizeof(loop));
    RECT pendingRect{0,0,width,height};uint64_t pendingDelay=0,totalArea=0;size_t written=0;
    auto write_pending=[&](){
        uint64_t delay=pendingDelay/10;
        bool repeated=false;
        do{
            const auto& block=repeated?hold:pending;
            RECT area=repeated?RECT{0,0,1,1}:pendingRect;
            UINT chunk=(UINT)std::min<uint64_t>(delay,65535);
            const BYTE gce[]={0x21,0xf9,4,(BYTE)(4|(block.transparent?1:0))};
            bytes(gce,sizeof(gce));word(chunk);byte(block.transparent?255:0);byte(0);
            byte(0x2c);word(area.left);word(area.top);word(area.right-area.left);word(area.bottom-area.top);
            bool local=block.palette!=globalPalette;
            byte((local?0x80:0)|block.interlace|(local?block.size:0));
            if(local)bytes(block.palette.data(),block.palette.size());
            bytes(block.data.data(),block.data.size());
            totalArea+=(uint64_t)(area.right-area.left)*(area.bottom-area.top);
            ++written;delay-=chunk;repeated=true;
        }while(delay);
    };
    bool ok=true;uint64_t time=0;size_t event=1;
    while(time<duration){
        uint64_t next=timeline[event++];
        if(next<=time){ok=false;break;}
        pendingDelay+=next-time;time=next;
        if(time==duration)break;
        if(active.right==active.left||active.bottom==active.top)continue;
        auto patch=scene.render(time,&active);std::vector<BYTE> pixels;
        if(!patch||!bitmap_pixels(patch.get(),pixels)){ok=false;break;}
        int pw=active.right-active.left,ph=active.bottom-active.top;
        std::vector<BYTE> changes((size_t)pw*ph,0);
        RECT changed{pw,ph,0,0};
        for(int y=0;y<ph;++y)for(int x=0;x<pw;++x){
            size_t source=((size_t)y*pw+x)*4;
            size_t target=((size_t)(y+active.top)*width+x+active.left)*4;
            if(memcmp(pixels.data()+source,previous.data()+target,3)){
                changes[(size_t)y*pw+x]=1;
                changed.left=std::min(changed.left,(LONG)x);changed.top=std::min(changed.top,(LONG)y);
                changed.right=std::max(changed.right,(LONG)x+1);changed.bottom=std::max(changed.bottom,(LONG)y+1);
                memcpy(previous.data()+target,pixels.data()+source,4);
            }
        }
        if(changed.right<=changed.left||changed.bottom<=changed.top)continue;
        write_pending();
        if(memcmp(holdColor,previous.data(),3)){
            corner->SetPixel(0,0,Color(255,previous[2],previous[1],previous[0]));
            cornerIndexed=quantizer.apply(corner.get());
            if(!cornerIndexed||!encode_block(cornerIndexed.get(),hold)){ok=false;break;}
            memcpy(holdColor,previous.data(),3);
        }
        Rect cropRect((int)changed.left,(int)changed.top,(int)(changed.right-changed.left),(int)(changed.bottom-changed.top));
        std::unique_ptr<Bitmap> crop(patch->Clone(cropRect,PixelFormat32bppRGB));
        std::vector<BYTE> croppedChanges((size_t)cropRect.Width*cropRect.Height);
        for(int y=0;y<cropRect.Height;++y)memcpy(croppedChanges.data()+(size_t)y*cropRect.Width,
            changes.data()+(size_t)(y+cropRect.Y)*pw+cropRect.X,cropRect.Width);
        indexed=crop?quantizer.apply(crop.get(),&croppedChanges):nullptr;
        if(!indexed||!encode_block(indexed.get(),pending)){ok=false;break;}
        pending.transparent=true;
        OffsetRect(&changed,active.left,active.top);pendingRect=changed;pendingDelay=0;
    }
    if(ok){write_pending();byte(0x3b);file.flush();ok=file.good();}
    file.close();
    if(ok)ok=MoveFileExW(temporary,path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok){DeleteFileW(temporary);logf(L"动态 GIF 编码失败：未完成文件已移除，原截图与已有文件保留");}
    else logf(L"动态 GIF 编码返回：帧=%zu 时长=%llums 编码像素=%llu 完整帧像素=%llu 耗时=%llums 文件=%s",
        written,duration,totalArea,(uint64_t)width*height*written,GetTickCount64()-started,path.c_str());
    return ok;
    }catch(const std::exception& error){
        if(temporary[0])DeleteFileW(temporary);
        logf(L"动态 GIF 编码异常：%s 耗时=%llums，保留原截图与已有文件",
            u8_to_w(error.what()).c_str(),GetTickCount64()-started);
        return false;
    }
}
