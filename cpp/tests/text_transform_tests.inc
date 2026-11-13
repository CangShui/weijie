static bool test_text_transforms(const std::filesystem::path& output){
    LogTrace trace;double originalScale=g_scale;bool passed=true;int checks=0;ensure_class();
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"文字框变换回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
    };
    auto distance=[](PointF a,PointF b){return hypotf(a.X-b.X,a.Y-b.Y);};
    for(double scale:{1.0,1.75}){
        g_scale=scale;Overlay o;o.traceId=trace.id();o.hasSel=true;o.sel={60,60,1260,860};
        o.tool=T_TEXT;o.toolSizes[T_TEXT-T_RECT]=24;
        o.shot.vw=1400;o.shot.vh=1100;o.shot.vx=-4000;o.shot.vy=-3000;
        o.shot.bmp=new Bitmap(1400,1100,PixelFormat32bppRGB);
        {Graphics g(o.shot.bmp);g.Clear(Color::White);}
        HWND form=CreateWindowW(OV_CLASS,L"text-transform-test",WS_POPUP,
            -4000,-3000,1400,1100,nullptr,nullptr,g_hinst,&o);
        if(!form){passed=false;continue;}
        stampTestForeground=form;
        auto client=[&](PointF p){return POINT{(LONG)lround(p.X)+o.sel.left,(LONG)lround(p.Y)+o.sel.top};};
        auto send=[&](UINT message,POINT p,WPARAM keys=0){SendMessageW(form,message,keys,MAKELPARAM(p.x,p.y));};
        auto drag=[&](int handle,PointF delta){
            Ann text;if(!selected_text(&o,text))return false;
            POINT begin=client(text_handle(text,handle));
            send(WM_LBUTTONDOWN,begin,MK_LBUTTON);
            POINT end{begin.x+(LONG)lround(delta.X),begin.y+(LONG)lround(delta.Y)};
            send(WM_MOUSEMOVE,end,MK_LBUTTON);send(WM_LBUTTONUP,end);
            return true;
        };
        send(WM_LBUTTONDOWN,{240,240},MK_LBUTTON);
        report(o.editing&&o.anns.empty(),"new text starts as editable draft rather than committed annotation");
        for(wchar_t c:std::wstring(L"测试 Aa"))SendMessageW(form,WM_CHAR,c,0);
        Ann draft=editing_text(&o);
        auto draftSize=draft.text_frame_size();
        POINT draftBody=client(draft.text_point({draftSize.Width/2,draftSize.Height/2}));
        set_cursor_for(&o,draftBody);
        report(GetCursor()==LoadCursorW(nullptr,IDC_ARROW)&&o.editing,
            "editing text body uses arrow while keyboard input and caret remain active");
        bool handles=true;
        bool handleCursors=true;
        for(int i=0;i<9;++i){
            POINT point=client(text_handle(draft,i));
            handles=handles&&text_frame_hit(&o,point)==i;
            set_cursor_for(&o,point);
            handleCursors=handleCursors&&GetCursor()==LoadCursorW(nullptr,text_handle_cursor(draft,i));
        }
        report(handles,"editing draft exposes eight resize handles and one rotation handle");
        report(handleCursors,"editing resize and rotation handles retain their operation cursors instead of arrow or I-beam");
        auto natural=draft.text_frame_size();
        drag(4,{SF(70),SF(25)});
        report(o.editing&&o.editingIndex==0&&o.anns.size()==1&&
            fabs(o.anns[0].text_frame_size().Width-natural.Width-SF(70))<1.5f&&
            fabs(o.anns[0].text_frame_size().Height-natural.Height-SF(25))<1.5f,
            "corner stretch changes width and height independently while resuming text editing");
        Ann stretched=o.anns[0];
        undo(&o);report(o.anns.size()==1&&o.anns[0].textScaleX==1&&o.anns[0].textScaleY==1,
            "undo treats draft stretch as one change without deleting text");
        redo(&o);report(same_text(o.anns[0],stretched),"redo restores stretched geometry");
        o.selectedText=0;auto size=o.anns[0].text_frame_size();
        PointF center=o.anns[0].text_point({size.Width/2,size.Height/2});
        POINT begin=client(text_handle(o.anns[0],8));
        PointF vector((REAL)(begin.x-o.sel.left)-center.X,(REAL)(begin.y-o.sel.top)-center.Y);
        POINT end=client({center.X-vector.Y,center.Y+vector.X});
        send(WM_LBUTTONDOWN,begin,MK_LBUTTON);send(WM_MOUSEMOVE,end,MK_LBUTTON);send(WM_LBUTTONUP,end);
        auto rotated=o.anns[0];
        report(fabs(rotated.textAngle-90)<2&&
            distance(rotated.text_point({size.Width/2,size.Height/2}),center)<.01f,
            "rotation handle turns text freely around fixed center");
        undo(&o);report(fabs(o.anns[0].textAngle)<.01f,"rotation is independently undoable");
        redo(&o);o.selectedText=-1;
        auto text=o.anns[0];size=text.text_frame_size();center=text.text_point({size.Width/2,size.Height/2});
        POINT body=client(center);set_cursor_for(&o,body);
        report(GetCursor()==LoadCursorW(nullptr,IDC_ARROW),"hovering finished text keeps ordinary arrow for accurate left-button placement");
        RECT selection=o.sel;send(WM_RBUTTONDOWN,body,MK_RBUTTON);
        end={body.x+53,body.y-17};send(WM_MOUSEMOVE,end,MK_RBUTTON);send(WM_RBUTTONUP,end);
        report(fabs(o.anns[0].pos.X-text.pos.X-53)<.01f&&fabs(o.anns[0].pos.Y-text.pos.Y+17)<.01f&&
            EqualRect(&selection,&o.sel)&&o.anns.size()==1,"dragging text moves only text and never the screenshot selection");
        Ann moved=o.anns[0];undo(&o);
        report(same_text(o.anns[0],text),"move undo restores previous position");
        redo(&o);report(same_text(o.anns[0],moved),"move redo preserves angle and stretch");
        size=moved.text_frame_size();body=client(moved.text_point({size.Width/2,size.Height/2}));
        int copies=overlayCopies;send(WM_LBUTTONDBLCLK,body);
        report(o.editing&&o.editingIndex==0&&o.editText==moved.text&&overlayCopies==copies,
            "double click reopens existing text without copying or adding a new object");
        o.editText=L"重新编辑";SendMessageW(form,WM_CHAR,L'\r',0);
        report(!o.editing&&o.anns.size()==1&&o.anns[0].text==L"重新编辑"&&
            o.anns[0].textAngle==moved.textAngle&&o.anns[0].textScaleX==moved.textScaleX&&
            distance(o.anns[0].pos,moved.pos)<.01f,"text content editing preserves transform and object position");
        undo(&o);report(o.anns[0].text==moved.text,"content modification is undoable separately from transforms");
        redo(&o);
        begin_text(&o,{},0);o.editText=L"取消编辑";cancel_text(&o);
        report(o.anns[0].text==L"重新编辑"&&!o.editing,"cancel editing retains original text content");
        begin_text(&o,{},0);
        Ann beforeBorderMove=editing_text(&o);
        auto corner=text_handle(beforeBorderMove,0),top=text_handle(beforeBorderMove,1);
        POINT border=client({(corner.X+top.X)/2,(corner.Y+top.Y)/2});
        set_cursor_for(&o,border);
        report(text_frame_hit(&o,border)==9&&GetCursor()==LoadCursorW(nullptr,IDC_SIZEALL),
            "editing frame border keeps move cursor for easy dragging");
        send(WM_LBUTTONDOWN,border,MK_LBUTTON);
        POINT borderEnd{border.x+35,border.y+20};
        send(WM_MOUSEMOVE,borderEnd,MK_LBUTTON);send(WM_LBUTTONUP,borderEnd);
        report(o.editing&&o.anns.size()==1&&o.editText==beforeBorderMove.text&&
            fabs(o.editPos.X-beforeBorderMove.pos.X-35)<.01f&&
            fabs(o.editPos.Y-beforeBorderMove.pos.Y-20)<.01f,
            "dragging editing frame border moves text and resumes editing without changing content");
        undo(&o);
        for(int i=0;i<8;++i){
            Ann before;before.kind=AK_TEXT;before.text=L"控制点 Aa";before.fontSize=24;
            before.pos={SF(260),SF(180)};before.textScaleX=1.2f;before.textScaleY=.9f;before.textAngle=31;
            o.anns[0]=before;o.selectedText=0;o.tool=T_TEXT;
            auto anchor=text_handle(before,(i+4)%8);
            drag(i,{SF(14),SF(11)});
            report(distance(anchor,text_handle(o.anns[0],(i+4)%8))<.05f&&
                o.anns[0].textScaleX>0&&o.anns[0].textScaleY>0&&o.anns[0].textAngle==31,
                "rotated resize keeps opposite corner or edge anchored");
        }
        Ann before=o.anns[0];o.selectedText=0;
        begin=client(text_handle(before,4));send(WM_LBUTTONDOWN,begin,MK_LBUTTON);
        send(WM_MOUSEMOVE,{begin.x+60,begin.y+40},MK_LBUTTON);
        SendMessageW(form,WM_KEYDOWN,VK_ESCAPE,0);
        report(!o.textDrag.kind&&same_text(o.anns[0],before)&&GetCapture()!=form&&o.alive,
            "Escape cancels stretch without closing screenshot or changing text");
        send(WM_LBUTTONDOWN,begin,MK_LBUTTON);send(WM_MOUSEMOVE,{begin.x+60,begin.y},MK_LBUTTON);
        HWND other=CreateWindowW(L"STATIC",L"capture-owner",WS_POPUP,-5000,-3000,10,10,nullptr,nullptr,g_hinst,nullptr);
        SetCapture(other);
        report(!o.textDrag.kind&&same_text(o.anns[0],before)&&GetCapture()==other,
            "capture transfer rolls back transform without releasing new owner's capture");
        ReleaseCapture();DestroyWindow(other);
        o.selectedText=0;drag(4,{-SF(2000),-SF(2000)});
        report(o.anns[0].text_frame_size().Width>=SF(8)-.01f&&o.anns[0].text_frame_size().Height>=SF(8)-.01f,
            "crossing opposite handle clamps to positive readable geometry instead of flipping");
        undo(&o);
        Ann exported=before;exported.pos={SF(130),SF(110)};exported.textScaleX=1.6f;exported.textScaleY=.65f;exported.textAngle=37;
        o.anns={exported};o.selectedText=0;
        std::unique_ptr<Bitmap> actual(render_result(&o));Bitmap expected(1200,800,PixelFormat32bppRGB);
        {
            Graphics g(&expected);g.Clear(Color::White);g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);g.SetPixelOffsetMode(PixelOffsetModeHalf);
            float radians=exported.textAngle*3.14159265359f/180,cs=cosf(radians),sn=sinf(radians);
            Matrix transform(exported.textScaleX*cs,exported.textScaleX*sn,-exported.textScaleY*sn,
                exported.textScaleY*cs,exported.pos.X,exported.pos.Y);g.SetTransform(&transform);
            Font font(FONT_UI(),SF(exported.fontSize),FontStyleRegular,UnitPixel);SolidBrush ink(exported.color);
            g.DrawString(exported.text.c_str(),-1,&font,PointF(0,0),nullptr,&ink);
        }
        report(actual&&same_stamp_pixels(actual.get(),&expected),"export matches independent affine matrix without editing frame or rotation handles");
        AnimatedScreenshot scene;scene.background.reset(o.shot.bmp->Clone(Rect(60,60,1200,800),PixelFormat32bppRGB));
        scene.annotations=o.anns;scene.scale=g_scale;
        auto composed=scene.render(0);RECT crop{80,50,650,600};auto region=scene.render(0,&crop);
        std::unique_ptr<Bitmap> part(expected.Clone(Rect(80,50,570,550),PixelFormat32bppRGB));
        report(composed&&same_stamp_pixels(composed.get(),&expected)&&region&&same_stamp_pixels(region.get(),part.get()),
            "GIF and visible-region rendering preserve transformed text identically");
        auto animation=decode_stamp_animation((std::filesystem::path(__FILE__).parent_path()/L"fixtures"/L"gif"/L"short.gif"));
        Ann sticker;sticker.kind=AK_EMOJI;sticker.emojiAnimation=animation;
        sticker.emojiImage=animation?animation->frames.front():nullptr;sticker.fontSize=120;sticker.pos={SF(230),SF(180)};
        scene.annotations.push_back(sticker);
        auto folder=output/L"text-transform-export";std::filesystem::create_directories(folder);
        auto suffix=scale==1?L"100":L"175";
        auto filename=folder/(std::wstring(L"combined-")+suffix+L".gif");
        report(animation&&save_animated_gif(scene,filename.wstring()),
            "animated GIF export includes stretched rotated text and overlapping animated sticker");
        for(uint64_t time:{0ull,70ull}){
            auto frame=scene.render(time);
            save_bitmap_file(frame.get(),(folder/(std::wstring(L"combined-")+suffix+L"-"+std::to_wstring(time)+L".png")).wstring(),92);
        }
        begin_text(&o,{},0);o.editText.clear();commit_text(&o);
        report(o.anns.empty(),"committing empty existing text removes the object");
        undo(&o);report(o.anns.size()==1&&same_text(o.anns[0],exported),"undo restores deleted text and all transforms");
        redo(&o);report(o.anns.empty(),"redo removes the edited-empty text again");
        o.undoStack.clear();o.redoStack.clear();
        begin_text(&o,{300,300});Ann empty=editing_text(&o);
        drag(4,{SF(60),SF(20)});
        report(o.editing&&o.anns.empty()&&o.textDraft.textScaleX>1&&o.textDraft.textScaleY>1,
            "empty draft box may be stretched before entering characters");
        cancel_text(&o);report(o.anns.empty(),"discarding stretched empty box does not create empty annotations");
        o.anns={exported};o.undoStack.clear();o.redoStack.clear();
        begin_text(&o,{900,650});o.editText=L"第二个";commit_text(&o);
        o.selectedText=0;before=o.anns[0];size=before.text_frame_size();body=client(before.text_point({size.Width/2,size.Height/2}));
        send(WM_RBUTTONDOWN,body,MK_RBUTTON);send(WM_MOUSEMOVE,{body.x+35,body.y+25},MK_RBUTTON);
        send(WM_RBUTTONUP,{body.x+35,body.y+25});Ann final=o.anns[0];
        undo(&o);undo(&o);
        report(o.anns.size()==1&&same_text(o.anns[0],before),"undo ordering distinguishes move from later text insertion");
        redo(&o);redo(&o);
        report(o.anns.size()==2&&same_text(o.anns[0],final),"redo ordering restores insertion and text move without duplicating objects");
        o.anns={exported};o.undoStack.clear();o.redoStack.clear();o.selectedText=0;
        auto bounds=exported.bounds();
        PointF points[]={exported.text_point({0,0}),exported.text_point({exported.text_frame_size().Width,0}),
            exported.text_point({exported.text_frame_size().Width,exported.text_frame_size().Height}),
            exported.text_point({0,exported.text_frame_size().Height})};
        bool contains=true;for(auto point:points)contains=contains&&point.X>=bounds.X-.1f&&point.X<=bounds.GetRight()+.1f&&
            point.Y>=bounds.Y-.1f&&point.Y<=bounds.GetBottom()+.1f;
        report(contains&&!exported.text_contains({bounds.X+1,bounds.Y+1}),"rotated hit testing uses inverse transform rather than axis-aligned bounding box");
        begin_text(&o,{1100,700});
        size=exported.text_frame_size();body=client(exported.text_point({size.Width/2,size.Height/2}));
        set_cursor_for(&o,body);
        report(GetCursor()==LoadCursorW(nullptr,IDC_ARROW),"existing text does not hijack pointer while an empty draft exists elsewhere");
        cancel_text(&o);o.selectedText=0;
        set_tool(&o,T_RECT);send(WM_LBUTTONDOWN,body,MK_LBUTTON);
        report(o.mode==4&&o.hasCur&&o.cur.kind==AK_RECT,"drawing tools can annotate over text instead of unexpectedly moving it");
        send(WM_LBUTTONUP,{body.x+20,body.y+20});undo(&o);
        report(o.anns.size()==1&&same_text(o.anns[0],exported),"undo drawing over text retains original transformed text");
        o.tool=T_TEXT;o.selectedText=0;
        Bitmap preview(1200,800,PixelFormat32bppRGB);
        {Graphics g(&preview);g.Clear(Color(255,247,248,247));g.SetSmoothingMode(SmoothingModeAntiAlias);
         g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);exported.draw(g,nullptr);draw_text_frame(g,&o);}
        report(save_bitmap_file(&preview,(output/(scale==1?L"text-transform-100.png":L"text-transform-175.png")).wstring(),92),
            "rotated stretched text with frame and rotation handle snapshot rendered");
        begin=client(text_handle(exported,4));send(WM_LBUTTONDOWN,begin,MK_LBUTTON);
        DestroyWindow(form);report(!o.textDrag.kind&&GetCapture()!=form,"closing overlay during text resize releases transform capture");
        stampTestForeground=nullptr;
    }
    g_scale=originalScale;std::cout<<"Text transform checks: "<<checks<<std::endl;return passed;
}
