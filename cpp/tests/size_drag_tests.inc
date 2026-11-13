static bool same_stamp_pixels(Bitmap* actual,Bitmap* expected);

static bool size_drag_snapshot(Overlay* overlay,int tool,const std::wstring& path){
    auto previous=overlay->sizeDrag;
    overlay->sizeDrag={};auto& drag=overlay->sizeDrag;
    drag.active=true;drag.tool=tool;drag.bounds={0,0,S(280),S(76)};
    drag.length=SF(248);
    drag.thumb=drag.length*(overlay->toolSizes[tool-T_RECT]-tool_size_minimum(tool))/
        (float)(tool_size_maximum(tool)-tool_size_minimum(tool));
    Bitmap image(S(280),S(76),PixelFormat32bppARGB);
    {
        Graphics graphics(&image);graphics.Clear(Color::White);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        draw_size_drag(graphics,overlay);
    }
    bool ok=save_bitmap_file(&image,path,92);overlay->sizeDrag=previous;return ok;
}

static bool test_size_drag(){
    double originalScale=g_scale;bool passed=true;int checks=0;ensure_class();
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"右键尺寸手势回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
    };
    for(double scale:{1.0,1.75})for(int tool=T_RECT;tool<=T_EMOJI;++tool){
        g_scale=scale;LogTrace trace;Overlay overlay;overlay.traceId=trace.id();overlay.hasSel=true;
        overlay.sel={100,100,1500,900};overlay.tbVisible=true;overlay.tbX=80;overlay.tbY=1000;
        overlay.shot.vw=1800;overlay.shot.vh=1400;overlay.shot.vx=-4000;overlay.shot.vy=-3000;
        overlay.shot.bmp=new Bitmap(1800,1400,PixelFormat32bppRGB);
        {Graphics g(overlay.shot.bmp);g.Clear(Color::White);}
        HWND form=CreateWindowW(OV_CLASS,L"right-size-test",WS_POPUP,-4000,-3000,1800,1400,
            nullptr,nullptr,g_hinst,&overlay);
        if(!form){passed=false;continue;}
        stampTestForeground=form;overlay.tool=T_NONE;
        int minimum=tool_size_minimum(tool),maximum=tool_size_maximum(tool);
        int initial=minimum+(maximum-minimum)/3;overlay.toolSizes[tool-T_RECT]=initial;
        Ann existing;existing.kind=AK_NUMBER;existing.diameter=31;existing.index=1;existing.pos={80,80};overlay.anns.push_back(existing);
        std::unique_ptr<Bitmap> clean(render_result(&overlay));
        POINT origin{};
        for(const auto& item:overlay.items)if(item.id==tool)
            origin={(LONG)lround(overlay.tbX+item.rect.X+item.rect.Width/2),
                    (LONG)lround(overlay.tbY+item.rect.Y+item.rect.Height/2)};
        size_t imageCount=overlay.anns.size();
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
        report(overlay.sizeDrag.active&&overlay.sizeDrag.tool==tool&&overlay.tool==tool&&
               GetCapture()==form&&!overlay.stylePopup&&!overlay.emojiPopup,
               "right button down selects tool and captures pointer without opening a size window");
        report(overlay.toolSizes[tool-T_RECT]==initial&&
               overlay.sizeDrag.bounds.left>=0&&overlay.sizeDrag.bounds.top>=0&&
               overlay.sizeDrag.bounds.right<=overlay.shot.vw&&overlay.sizeDrag.bounds.bottom<=overlay.shot.vh,
               "press starts at current size and keeps inline slider within desktop");
        float before=overlay.sizeDrag.thumb;bool follows=true,notWholeCanvas=true;
        for(int dx=1;dx<=5;++dx){
            ValidateRect(form,nullptr);
            SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(origin.x+dx,origin.y));
            follows=follows&&fabs(overlay.sizeDrag.thumb-before-dx)<.01f;
            RECT dirty{};
            if(GetUpdateRect(form,&dirty,FALSE)){
                RECT outside{};
                notWholeCanvas=notWholeCanvas&&IntersectRect(&outside,&dirty,&overlay.sizeDrag.bounds)&&
                    dirty.left>=overlay.sizeDrag.bounds.left&&dirty.top>=overlay.sizeDrag.bounds.top&&
                    dirty.right<=overlay.sizeDrag.bounds.right&&dirty.bottom<=overlay.sizeDrag.bounds.bottom;
            }
        }
        report(follows&&notWholeCanvas,"thumb follows each mouse pixel without invalidating the full screenshot");
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(origin.x-3000,origin.y-1500));
        report(overlay.toolSizes[tool-T_RECT]==minimum&&overlay.sizeDrag.active&&GetCapture()==form,
               "dragging outside overlay still adjusts and clamps to tool minimum");
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(origin.x+3000,origin.y+1500));
        report(overlay.toolSizes[tool-T_RECT]==maximum&&overlay.sizeDrag.thumb==overlay.sizeDrag.length&&
               overlay.anns.size()==imageCount&&overlay.anns[0].diameter==31,
               "rightward drag reaches maximum without adding or resizing existing annotations");
        std::unique_ptr<Bitmap> exported(render_result(&overlay));
        report(clean&&exported&&same_stamp_pixels(clean.get(),exported.get()),
               "inline slider and size badge never enter exported screenshot");
        SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON|MK_RBUTTON,MAKELPARAM(500,500));
        SendMessageW(form,WM_LBUTTONUP,MK_RBUTTON,MAKELPARAM(500,500));
        report(overlay.anns.size()==imageCount&&overlay.sizeDrag.active,
               "left clicks during right sizing cannot accidentally place annotations");
        SendMessageW(form,WM_RBUTTONUP,0,MAKELPARAM(origin.x+3000,origin.y));
        report(!overlay.sizeDrag.active&&GetCapture()!=form&&!overlay.stylePopup&&
               overlay.toolSizes[tool-T_RECT]==maximum&&overlay.alive,
               "right release confirms size and hides inline slider without opening a panel");
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(origin.x-3000,origin.y));
        SendMessageW(form,WM_KEYDOWN,VK_ESCAPE,0);
        report(!overlay.sizeDrag.active&&GetCapture()!=form&&overlay.toolSizes[tool-T_RECT]==maximum&&IsWindow(form),
               "Escape cancels current adjustment and restores size without closing screenshot");
        SendMessageW(form,WM_RBUTTONUP,0,MAKELPARAM(origin.x,origin.y));
        report(!overlay.stylePopup&&!overlay.sizeDrag.active,
               "release after cancellation is consumed instead of reopening a size window");
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(origin.x-3000,origin.y));
        HWND other=CreateWindowW(L"STATIC",L"capture-transfer",WS_POPUP,-4000,-3000,10,10,
            nullptr,nullptr,g_hinst,nullptr);SetCapture(other);
        report(!overlay.sizeDrag.active&&overlay.toolSizes[tool-T_RECT]==maximum&&GetCapture()==other,
               "capture transfer cancels size adjustment and preserves new capture owner");
        ReleaseCapture();DestroyWindow(other);
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(origin.x-3000,origin.y));
        SendMessageW(form,WM_KILLFOCUS,0,0);
        report(!overlay.sizeDrag.active&&overlay.toolSizes[tool-T_RECT]==maximum&&GetCapture()!=form,
               "focus loss releases drag and restores original size");
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(origin.x-3000,origin.y));
        SendMessageW(form,WM_MOUSEMOVE,0,MAKELPARAM(origin.x-3000,origin.y));
        report(!overlay.sizeDrag.active&&overlay.toolSizes[tool-T_RECT]==minimum&&GetCapture()!=form,
               "missing mouse-up is recovered from button state and keeps last selected size");
        SendMessageW(form,WM_RBUTTONDBLCLK,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
        report(overlay.sizeDrag.active,"second press in a rapid right double click also starts sizing");
        SendMessageW(form,WM_CANCELMODE,0,0);
        report(!overlay.sizeDrag.active&&GetCapture()!=form,
               "cancel mode releases pointer capture and removes sizing strip");
        if(tool==T_RECT){
            HWND edit=CreateWindowW(L"EDIT",L"result",WS_CHILD,0,0,100,100,form,nullptr,g_hinst,nullptr);
            SetFocus(edit);
            SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
            report(GetFocus()==form&&overlay.sizeDrag.active,"right hold returns keyboard focus from result text to screenshot");
            SendMessageW(form,WM_KEYDOWN,VK_ESCAPE,0);DestroyWindow(edit);
        }
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(origin.x,origin.y));
        DestroyWindow(form);
        report(!overlay.alive&&!overlay.sizeDrag.active&&GetCapture()!=form,
               "closing screenshot during held right button releases all sizing state");
        stampTestForeground=nullptr;
    }
    g_scale=originalScale;std::cout<<"Right size drag checks: "<<checks<<std::endl;return passed;
}
