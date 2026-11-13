#include "desktop_duplication.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>
#include "capture_frame_wait.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

bool copy_duplication_pixels(const BYTE* src, UINT width, UINT height, UINT pitch,
                             DXGI_MODE_ROTATION rotation, const RECT& output,
                             BYTE* dst, const RECT& desktop) {
    bool swapped = rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                   rotation == DXGI_MODE_ROTATION_ROTATE270;
    if (!src || !dst || !width || !height || pitch < (size_t)width * 4 ||
        output.right - output.left != (LONG)(swapped ? height : width) ||
        output.bottom - output.top != (LONG)(swapped ? width : height) ||
        desktop.right <= desktop.left || desktop.bottom <= desktop.top)
        return false;
    RECT part{};
    if (!IntersectRect(&part, &desktop, &output)) return true;
    size_t stride = (size_t)(desktop.right - desktop.left) * 4;
    for (LONG y = part.top; y < part.bottom; ++y) {
        BYTE* row = dst + (size_t)(y - desktop.top) * stride +
                    (size_t)(part.left - desktop.left) * 4;
        UINT oy = y - output.top;
        if (rotation == DXGI_MODE_ROTATION_IDENTITY ||
            rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
            std::memcpy(row, src + (size_t)oy * pitch +
                        (size_t)(part.left - output.left) * 4,
                        (size_t)(part.right - part.left) * 4);
            continue;
        }
        for (LONG x = part.left; x < part.right; ++x, row += 4) {
            UINT ox = x - output.left, sx = 0, sy = 0;
            switch (rotation) {
            case DXGI_MODE_ROTATION_ROTATE90: sx = oy; sy = height - 1 - ox; break;
            case DXGI_MODE_ROTATION_ROTATE180: sx = width - 1 - ox; sy = height - 1 - oy; break;
            case DXGI_MODE_ROTATION_ROTATE270: sx = width - 1 - oy; sy = ox; break;
            default: return false;
            }
            std::memcpy(row, src + (size_t)sy * pitch + (size_t)sx * 4, 4);
        }
    }
    return true;
}

struct CaptureDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};
struct CaptureDeviceCache {
    ComPtr<IDXGIFactory1> factory;
    std::vector<CaptureDevice> adapters;
};
// 只复用设备，不保留桌面帧、staging 纹理或 duplication；空闲时不轮询屏幕。
static thread_local CaptureDeviceCache captureDevices;

struct CaptureOutput {
    DXGI_OUTPUT_DESC desc{};
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result=E_PENDING;
    UINT pointerUpdates=0;
};

static HRESULT read_desktop_image(CaptureOutput& output,IDXGIResource* resource,
                                  BYTE* bits,const RECT& desktop){
    ComPtr<ID3D11Texture2D> frame;
    HRESULT hr=resource->QueryInterface(IID_PPV_ARGS(&frame));
    if(FAILED(hr))return hr;
    D3D11_TEXTURE2D_DESC texture{};
    frame->GetDesc(&texture);
    if(texture.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)return DXGI_ERROR_UNSUPPORTED;
    texture.Usage=D3D11_USAGE_STAGING;texture.BindFlags=0;texture.MiscFlags=0;
    texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr=output.device->CreateTexture2D(&texture,nullptr,&staging);
    if(FAILED(hr))return hr;
    output.context->CopyResource(staging.Get(),frame.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr=output.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
    if(FAILED(hr))return hr;
    bool copied=copy_duplication_pixels((BYTE*)mapped.pData,texture.Width,texture.Height,
        mapped.RowPitch,output.desc.Rotation,output.desc.DesktopCoordinates,bits,desktop);
    output.context->Unmap(staging.Get(),0);
    return copied?S_OK:E_INVALIDARG;
}

static bool try_desktop_image(CaptureOutput& output,BYTE* bits,const RECT& desktop){
    if(output.result!=E_PENDING)return true;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr=output.duplication->AcquireNextFrame(0,&info,&resource);
    if(hr==DXGI_ERROR_WAIT_TIMEOUT)return false;
    if(FAILED(hr)){output.result=hr;return true;}
    if(info.LastPresentTime.QuadPart==0){
        ++output.pointerUpdates;
        hr=output.duplication->ReleaseFrame();
        if(FAILED(hr)){output.result=hr;return true;}
        return false;
    }
    auto started=GetTickCount64();
    output.result=read_desktop_image(output,resource.Get(),bits,desktop);
    hr=output.duplication->ReleaseFrame();
    if(FAILED(hr))
        logf(L"DXGI 帧释放返回异常：显示器=%s HRESULT=0x%08X，当前复制结果=0x%08X",
            output.desc.DeviceName,(unsigned)hr,(unsigned)output.result);
    logf(L"DXGI 桌面帧：显示器=%s 累积帧=%u present=%lld 光标通知=%u 保护遮蔽=%d 读取复制=%llums",
        output.desc.DeviceName,info.AccumulatedFrames,info.LastPresentTime.QuadPart,output.pointerUpdates,
        (int)info.ProtectedContentMaskedOut,GetTickCount64()-started);
    if(info.ProtectedContentMaskedOut)
        logf(L"抓屏提示：显示器=%s 含系统保护画面，受保护区域由系统遮蔽",output.desc.DeviceName);
    return true;
}

bool capture_desktop_dxgi(BYTE* bits,const RECT& desktop,bool gdiAvailable){
    const auto started=GetTickCount64();
    auto& cache=captureDevices;
    if(cache.factory&&!cache.factory->IsCurrent()){
        logf(L"DXGI 设备缓存失效：显示适配器列表已改变，释放旧设备并重新枚举");
        cache={};
    }
    if(!cache.factory){
        HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&cache.factory));
        if(FAILED(hr)){
            logf(L"DXGI 抓屏初始化失败 HRESULT=0x%08X，改用 GDI",(unsigned)hr);
            return false;
        }
    }
    int created=0,reused=0,duplicated=0;
    bool allCopied=true;
    std::vector<CaptureOutput> outputs;
    for(UINT a=0;;++a){
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr=cache.factory->EnumAdapters1(a,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(hr)){allCopied=false;break;}
        bool deviceAttempted=false;
        HRESULT deviceResult=E_PENDING;
        for(UINT n=0;;++n){
            ComPtr<IDXGIOutput> source;
            hr=adapter->EnumOutputs(n,&source);
            if(hr==DXGI_ERROR_NOT_FOUND)break;
            if(FAILED(hr)){allCopied=false;break;}
            CaptureOutput output;
            if(FAILED(source->GetDesc(&output.desc))){allCopied=false;continue;}
            RECT part{};
            if(!output.desc.AttachedToDesktop||
               !IntersectRect(&part,&desktop,&output.desc.DesktopCoordinates))continue;
            if(cache.adapters.size()<=a)cache.adapters.resize(a+1);
            auto& device=cache.adapters[a];
            if(!deviceAttempted){
                auto deviceStart=GetTickCount64();
                deviceAttempted=true;
                if(device.device&&SUCCEEDED(device.device->GetDeviceRemovedReason())){
                    deviceResult=S_OK;++reused;
                }else{
                    device={};
                    deviceResult=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
                        &device.device,nullptr,&device.context);
                    if(SUCCEEDED(deviceResult))++created;
                }
                logf(L"DXGI 设备准备：适配器=%u 新建=%d 复用=%d HRESULT=0x%08X 耗时=%llums",
                    a,created,reused,(unsigned)deviceResult,GetTickCount64()-deviceStart);
            }
            if(FAILED(deviceResult))output.result=deviceResult;
            else{
                output.device=device.device;output.context=device.context;
                ComPtr<IDXGIOutput1> output1;
                hr=source.As(&output1);
                if(SUCCEEDED(hr))hr=output1->DuplicateOutput(device.device.Get(),&output.duplication);
                if(FAILED(hr))output.result=hr;
            }
            outputs.push_back(std::move(output));
        }
    }
    auto prepared=GetTickCount64();
    wait_for_desktop_frames(outputs.size(),
        [&](size_t i){return try_desktop_image(outputs[i],bits,desktop);},
        [](){return GetTickCount64();},[](unsigned ms){Sleep(ms);});
    for(auto& output:outputs){
        if(output.result==E_PENDING)output.result=DXGI_ERROR_WAIT_TIMEOUT;
        if(SUCCEEDED(output.result)){
            ++duplicated;
            logf(L"显示器抓屏成功：方式=DXGI 显示器=%s 旋转=%u",output.desc.DeviceName,output.desc.Rotation);
        }else{
            allCopied=allCopied&&gdiAvailable;
            logf(L"显示器抓屏回退：显示器=%s DXGI HRESULT=0x%08X GDI基底=%s 光标通知=%u",
                output.desc.DeviceName,(unsigned)output.result,gdiAvailable?L"可用":L"不可用",output.pointerUpdates);
        }
    }
    logf(L"多屏抓屏返回：显示器=%zu DXGI成功=%d 全部完成=%d 设备新建=%d 复用=%d 准备=%llums 取帧复制=%llums 总计=%llums；所有屏共享300ms等待预算",
        outputs.size(),duplicated,(int)allCopied,created,reused,prepared-started,
        GetTickCount64()-prepared,GetTickCount64()-started);
    return !outputs.empty()&&allCopied;
}
