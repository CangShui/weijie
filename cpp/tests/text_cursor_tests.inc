static bool test_text_cursors(const std::filesystem::path& output){
    double originalScale=g_scale;HCURSOR originalCursor=GetCursor();bool passed=true;int checks=0;ensure_class();
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"文字字号光标回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
    };
    for(double scale:{1.0,1.75})for(int size:{10,18,48,96}){
        g_scale=scale;LogTrace trace;Overlay overlay;overlay.traceId=trace.id();
        overlay.hasSel=true;overlay.sel={50,50,850,650};overlay.tool=T_TEXT;
        overlay.toolSizes[T_TEXT-T_RECT]=size;overlay.shot.vw=900;overlay.shot.vh=800;
        overlay.shot.bmp=new Bitmap(900,800,PixelFormat32bppRGB);
        {Graphics g(overlay.shot.bmp);g.Clear(Color::White);}
        HWND form=CreateWindowW(OV_CLASS,L"text-cursor-test",WS_POPUP,
            -4000,-3000,900,800,nullptr,nullptr,g_hinst,&overlay);
        if(!form){passed=false;continue;}
        POINT point{400,350};set_cursor_for(&overlay,point);
        HCURSOR first=LoadCursorW(nullptr,IDC_ARROW);
        Bitmap measure(1,1);measure.SetResolution((REAL)(96*scale),(REAL)(96*scale));Graphics graphics(&measure);
        Font font(FONT_UI(),SF((float)size),FontStyleRegular,UnitPixel);
        report(GetCursor()==first,"text tool uses unmodified system arrow at every font size and DPI");
        set_cursor_for(&overlay,{420,360});
        report(GetCursor()==first&&overlay.anns.empty(),"moving text pointer retains system arrow without placing characters");
        auto caret=text_caret_rect({0,0},size);
        report(fabs(caret.Height-font.GetHeight(&graphics))<.01f&&caret.Width==SF(1.6f),
               "editing caret shares current font line height instead of fixed 18 pixels");
        {
            Overlay composing;composing.editing=true;composing.composing=L"拼";
            composing.editPos={20,20};composing.toolSizes[T_TEXT-T_RECT]=size;
            Bitmap image(400,400,PixelFormat32bppRGB);
            {Graphics g(&image);g.Clear(Color::White);g.SetSmoothingMode(SmoothingModeAntiAlias);draw_editing(g,&composing);}
            int underline=(int)lround(text_caret_rect(composing.editPos,size).GetBottom()+SF(1));
            bool visible=false;
            for(int y=underline-1;y<=underline+1;++y){
                Color pixel;image.GetPixel(22,y,&pixel);
                visible=visible||pixel.GetG()>pixel.GetR()+20;
            }
            report(visible,"IME composition underline follows scaled line bottom");
        }
        SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        report(overlay.editing&&overlay.editPos.X==point.x-50&&overlay.editPos.Y==point.y-50,
               "text cursor hotspot opens input at exact selection-local click position");
        report(GetCursor()==LoadCursorW(nullptr,IDC_ARROW),
               "entering text editing keeps ordinary arrow");
        set_cursor_for(&overlay,{700,500});
        report(GetCursor()==LoadCursorW(nullptr,IDC_ARROW),
               "editing keeps arrow over empty selection space without showing another placement I-beam");
        overlay.editText=L"示例 Aa";commit_text(&overlay);
        report(overlay.anns.size()==1&&overlay.anns[0].kind==AK_TEXT&&overlay.anns[0].fontSize==size,
               "committed text uses selected font size independently of mouse pointer");
        std::unique_ptr<Bitmap> exported(render_result(&overlay));
        Bitmap expected(800,600,PixelFormat32bppRGB);
        {
            Graphics g(&expected);g.Clear(Color::White);
            g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);overlay.anns[0].draw(g,nullptr);
        }
        report(exported&&same_stamp_pixels(exported.get(),&expected),"text export contains committed characters but no pointer or editing caret");
        point={700,600};
        int next=size==96?10:96;
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(point.x,point.y));
        float delta=(next-size)*overlay.sizeDrag.length/(tool_size_maximum(T_TEXT)-tool_size_minimum(T_TEXT));
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(point.x+(int)lround(delta),point.y));
        SendMessageW(form,WM_RBUTTONUP,0,MAKELPARAM(point.x+(int)lround(delta),point.y));
        set_cursor_for(&overlay,point);
        report(overlay.toolSizes[T_TEXT-T_RECT]==next&&GetCursor()==first&&overlay.anns[0].fontSize==size,
               "right sizing changes future font size but never scales or changes text pointer");
        SendMessageW(form,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(point.x,point.y));
        SendMessageW(form,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(point.x+100,point.y));
        SendMessageW(form,WM_KEYDOWN,VK_ESCAPE,0);set_cursor_for(&overlay,point);
        report(overlay.toolSizes[T_TEXT-T_RECT]==next&&GetCursor()==first,
               "Escape restores chosen font size while preserving system arrow");
        overlay.tbVisible=true;overlay.tbX=650;overlay.tbY=580;set_cursor_for(&overlay,point);
        bool toolbar=GetCursor()==first;overlay.tbVisible=false;
        overlay.panel=PM_TEXT;overlay.panelRect={650,580,180,120};set_cursor_for(&overlay,point);
        bool panel=GetCursor()==first;overlay.panel=PM_HIDDEN;
        set_cursor_for(&overlay,{49,350});bool outside=GetCursor()==first;
        set_cursor_for(&overlay,{50,50});
        report(toolbar&&panel&&outside&&GetCursor()==first,
               "toolbar result panel outside selection and selection corners retain precise system arrow");
        overlay.tbVisible=true;overlay.tbX=20;overlay.tbY=650;
        auto button=[&](int tool){
            for(const auto& item:overlay.items)if(item.id==tool)
                return POINT{(LONG)lround(overlay.tbX+item.rect.X+item.rect.Width/2),
                             (LONG)lround(overlay.tbY+item.rect.Y+item.rect.Height/2)};
            return POINT{};
        };
        POINT rectangle=button(T_RECT);
        SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(rectangle.x,rectangle.y));
        bool switched=overlay.tool==T_RECT;
        POINT textButton=button(T_TEXT);
        SendMessageW(form,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(textButton.x,textButton.y));
        set_cursor_for(&overlay,textButton);
        report(switched&&overlay.tool==T_TEXT&&!overlay.editing&&overlay.anns.size()==1&&GetCursor()==first,
               "actual toolbar clicks switch tools without placing text and retain precise system arrow");
        overlay.tbVisible=false;
        overlay.mode=2;overlay.edge=1;set_cursor_for(&overlay,point);
        report(GetCursor()==LoadCursorW(nullptr,IDC_SIZEWE),"active screenshot resize still has its operation cursor");
        overlay.mode=0;set_tool(&overlay,T_RECT);set_cursor_for(&overlay,point);
        report(GetCursor()==pointer_cursor(),
               "switching away from text restores ordinary tool cursor");
        DestroyWindow(form);
        report(!overlay.alive&&GetCapture()!=form,"closing text screenshot leaves no mouse capture");
    }
    for(double scale:{1.0,1.75}){
        g_scale=scale;LogTrace trace;Bitmap sheet(S(710),S(500),PixelFormat32bppRGB);
        {
            Graphics g(&sheet);g.Clear(Color(255,246,248,247));g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            Font label(FONT_UI(),SF(15),FontStyleBold,UnitPixel);SolidBrush ink(Color(255,40,55,45));
            g.DrawString(L"字号",-1,&label,PointF(SF(18),SF(14)),&ink);
            g.DrawString(L"普通箭头",-1,&label,PointF(SF(120),SF(14)),&ink);
            g.DrawString(L"输入竖线与实际文字",-1,&label,PointF(SF(305),SF(14)),&ink);
            float y=55;
            for(int size:{10,18,48,96}){
                auto caption=std::to_wstring(size)+L" px";
                g.DrawString(caption.c_str(),-1,&label,PointF(SF(18),SF(y)),&ink);
                Overlay overlay;overlay.traceId=trace.id();overlay.toolSizes[T_TEXT-T_RECT]=size;
                HCURSOR cursor=LoadCursorW(nullptr,IDC_ARROW);
                int height=(int)ceil(text_line_height(size));
                HDC dc=g.GetHDC();DrawIconEx(dc,S(145),S((int)y),cursor,0,0,0,nullptr,DI_NORMAL|DI_DEFAULTSIZE);g.ReleaseHDC(dc);
                overlay.editing=true;overlay.editText=L"Aa 文字";overlay.editPos={SF(310),SF(y)};
                draw_editing(g,&overlay);y+=std::max(50.0f,height/(float)scale+16);
            }
        }
        report(save_bitmap_file(&sheet,(output/(scale==1?L"text-cursors-100.png":L"text-cursors-175.png")).wstring(),92),
               "text cursor caret and text-size comparison snapshot rendered");
    }
    SetCursor(originalCursor);g_scale=originalScale;
    std::cout<<"Text cursor checks: "<<checks<<std::endl;return passed;
}
