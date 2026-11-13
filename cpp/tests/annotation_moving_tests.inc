static Ann moving_sample(AnnKind kind,PointF& hit){
    Ann ann;ann.kind=kind;ann.color=Color(255,220,35,65);ann.width=12;
    ann.rect={100,100,200,100};ann.a={100,100};ann.b={300,200};
    ann.pts={{100,150},{200,190},{300,120}};ann.pos={200,160};
    ann.diameter=50;ann.fontSize=24;ann.index=7;
    hit=kind==AK_RECT?PointF(100,150):kind==AK_ELLIPSE?PointF(300,150):
        kind==AK_ARROW?PointF(200,150):kind==AK_PEN||kind==AK_MOSAIC?PointF(200,190):ann.pos;
    if(kind==AK_TEXT){
        ann.text=L"移动 Aa";ann.pos={100,100};ann.textScaleX=1.4f;ann.textScaleY=.8f;ann.textAngle=31;
        auto size=ann.text_frame_size();hit=ann.text_point({size.Width/2,size.Height/2});
    }
    if(kind==AK_EMOJI){
        ann.fontSize=120;
        auto animation=std::make_shared<StampAnimation>();
        for(auto color:{Color(255,30,90,220),Color(255,230,170,20)}){
            auto frame=std::make_shared<Bitmap>(60,30,PixelFormat32bppARGB);
            {Graphics g(frame.get());g.Clear(color);}
            animation->frames.push_back(frame);
        }
        animation->delays={60,120};animation->duration=180;
        ann.emojiAnimation=animation;ann.emojiImage=animation->frames.front();
    }
    return ann;
}
static bool test_annotation_moving(const std::filesystem::path& output){
    double originalScale=g_scale;bool passed=true;int checks=0;ensure_class();
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"图案右键移动回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
    };
    for(double scale:{1.0,1.75}){
        g_scale=scale;LogTrace trace;Overlay o;o.traceId=trace.id();o.hasSel=true;
        o.sel={50,50,1050,750};o.shot.vw=1200;o.shot.vh=1000;o.shot.vx=-4000;o.shot.vy=-3000;
        o.shot.bmp=new Bitmap(1200,1000,PixelFormat32bppRGB);
        {Graphics g(o.shot.bmp);
         for(int y=0;y<1000;y+=20)for(int x=0;x<1200;x+=20){
            SolidBrush brush(Color(255,(BYTE)(x/20%2?65:220),(BYTE)(y/20%2?80:210),170));
            g.FillRectangle(&brush,x,y,20,20);
         }}
        HWND form=CreateWindowW(OV_CLASS,L"annotation-move-test",WS_POPUP,-4000,-3000,1200,1000,
            nullptr,nullptr,g_hinst,&o);
        if(!form)return false;stampTestForeground=form;
        auto send=[&](UINT message,POINT point,WPARAM keys=0){SendMessageW(form,message,keys,MAKELPARAM(point.x,point.y));};
        auto client=[&](PointF p){return POINT{(LONG)lround(p.X)+o.sel.left,(LONG)lround(p.Y)+o.sel.top};};
        auto prepare=[&](const std::vector<Ann>& anns){
            clear_annotation_selection(&o,L"回归场景切换");
            o.anns=anns;o.undoStack.clear();o.redoStack.clear();o.selectedText=-1;
            o.tool=T_RECT;o.tbVisible=false;o.panel=PM_HIDDEN;o.sizeRightConsumed=false;
        };
        auto noTransformFrame=[&](){
            Bitmap actual(1000,700,PixelFormat32bppRGB),blank(1000,700,PixelFormat32bppRGB);
            {Graphics g(&actual);g.Clear(Color::White);draw_text_frame(g,&o);}
            {Graphics g(&blank);g.Clear(Color::White);}
            return same_stamp_pixels(&actual,&blank);
        };
        for(int kind=AK_RECT;kind<=AK_EMOJI;++kind){
            PointF hit;Ann before=moving_sample((AnnKind)kind,hit);prepare({before});
            if(kind==AK_TEXT)o.tool=T_TEXT;
            if(kind==AK_MOSAIC)ensure_mosaic(&o);
            POINT start=client(hit),end{start.x+43,start.y+29};
            RECT selection=o.sel;int sizes[8];std::copy_n(o.toolSizes,8,sizes);
            send(WM_RBUTTONDOWN,start,MK_RBUTTON);
            report(o.annotationDrag.active&&!o.sizeDrag.active&&GetCapture()==form&&
                o.selectedAnnotation==0&&GetCursor()==LoadCursorW(nullptr,IDC_SIZEALL),
                "each annotation kind captures right button for movement rather than sizing");
            if(kind==AK_TEXT)
                report(o.selectedText==-1&&text_frame_hit(&o,client(text_handle(before,8)))==-1&&noTransformFrame(),
                    "right selecting rotated text draws no resize handles or rotation point");
            send(WM_MOUSEMOVE,{start.x+1,start.y+1},MK_RBUTTON);
            report(!o.annotationDrag.moved&&o.undoStack.empty(),
                "small pointer jitter keeps original geometry and creates no undo entry");
            send(WM_LBUTTONDOWN,start,MK_LBUTTON|MK_RBUTTON);send(WM_LBUTTONUP,start,MK_RBUTTON);
            report(o.anns.size()==1&&!o.hasCur&&o.annotationDrag.active,
                "left button during right drag cannot place another annotation");
            send(WM_MOUSEMOVE,end,MK_RBUTTON);send(WM_RBUTTONUP,end);
            Ann expected=before;
            // 独立调整几何坐标，避免用被测 translate 方法生成预期值。
            if(kind==AK_RECT||kind==AK_ELLIPSE){expected.rect.X+=43;expected.rect.Y+=29;}
            else if(kind==AK_ARROW){expected.a.X+=43;expected.a.Y+=29;expected.b.X+=43;expected.b.Y+=29;}
            else if(kind==AK_PEN||kind==AK_MOSAIC)for(auto& p:expected.pts){p.X+=43;p.Y+=29;}
            else {expected.pos.X+=43;expected.pos.Y+=29;}
            auto actual=o.anns[0].bounds(),wanted=expected.bounds();
            report(!o.annotationDrag.active&&GetCapture()!=form&&o.undoStack.size()==1&&
                fabs(actual.X-wanted.X)<.01f&&fabs(actual.Y-wanted.Y)<.01f&&
                fabs(actual.Width-wanted.Width)<.01f&&fabs(actual.Height-wanted.Height)<.01f&&
                EqualRect(&selection,&o.sel)&&std::equal(sizes,sizes+8,o.toolSizes),
                "right drag translates geometry without resizing selection tool or annotation");
            if(kind==AK_TEXT){
                report(!o.editing&&o.selectedText==-1&&o.selectedAnnotation==0&&
                    text_frame_hit(&o,client(text_handle(o.anns[0],4)))==-1&&noTransformFrame(),
                    "released right move retains only movement outline with no active transform hotspots");
                std::unique_ptr<Bitmap> snapshot(render_result(&o));
                {Graphics g(snapshot.get());g.SetSmoothingMode(SmoothingModeAntiAlias);
                 draw_text_frame(g,&o);draw_annotation_selection(g,&o);}
                report(save_bitmap_file(snapshot.get(),
                    (output/(scale==1?L"text-move-frame-100.png":L"text-move-frame-175.png")).wstring(),92),
                    "rotated text movement-only rectangle snapshot rendered");
            }
            std::unique_ptr<Bitmap> exported(render_result(&o));
            std::unique_ptr<Bitmap> reference(o.shot.bmp->Clone(Rect(50,50,1000,700),PixelFormat32bppRGB));
            {Graphics g(reference.get());g.SetSmoothingMode(SmoothingModeAntiAlias);
             g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);g.SetPixelOffsetMode(PixelOffsetModeHalf);
             expected.draw(g,o.mosaic);}
            report(exported&&same_stamp_pixels(exported.get(),reference.get()),
                "saved pixels use moved geometry and omit selection outline and hints");
            if(kind==AK_EMOJI){
                report(o.anns[0].emojiAnimation==before.emojiAnimation&&o.anns[0].index==before.index&&
                    o.anns[0].emojiAnimation->frame_index(100)==1,
                    "moving animated sticker preserves all frames timing and image ownership");
                AnimatedScreenshot scene;scene.scale=g_scale;scene.annotations=o.anns;
                scene.background.reset(o.shot.bmp->Clone(Rect(50,50,1000,700),PixelFormat32bppRGB));
                auto folder=output/L"annotation-moving-export";std::filesystem::create_directories(folder);
                auto suffix=scale==1?L"100":L"175";
                bool saved=save_animated_gif(scene,(folder/(std::wstring(L"moved-")+suffix+L".gif")).wstring());
                for(uint64_t time:{0ull,60ull}){
                    auto frame=scene.render(time);
                    saved=frame&&save_bitmap_file(frame.get(),
                        (folder/(std::wstring(L"moved-")+suffix+L"-"+std::to_wstring(time)+L".png")).wstring(),92)&&saved;
                }
                report(saved,"moved animated sticker saves GIF and independent per-frame reference images");
            }
            undo(&o);auto restored=o.anns[0].bounds(),original=before.bounds();
            report(o.undoStack.empty()&&fabs(restored.X-original.X)<.01f&&fabs(restored.Y-original.Y)<.01f,
                "one undo restores entire moved annotation");
            redo(&o);auto redone=o.anns[0].bounds();
            report(fabs(redone.X-wanted.X)<.01f&&fabs(redone.Y-wanted.Y)<.01f&&o.anns.size()==1,
                "redo restores movement without duplicating annotation");
        }
        PointF hit;Ann rectangle=moving_sample(AK_RECT,hit),ellipse=moving_sample(AK_ELLIPSE,hit);
        report(rectangle.hit_test({100,150},SF(3))&&!rectangle.hit_test({200,150},SF(3))&&
            ellipse.hit_test({300,150},SF(3))&&!ellipse.hit_test({200,150},SF(3)),
            "hollow rectangle and ellipse hit only outlines instead of stealing entire interior");
        Ann pen=moving_sample(AK_PEN,hit),image=moving_sample(AK_EMOJI,hit);
        report(pen.hit_test({200,190},SF(3))&&!pen.hit_test({130,120},SF(3))&&
            !image.hit_test({200,160+SF(45)},SF(3)),
            "stroke gaps and sticker aspect-ratio padding remain click-through");
        Ann wideArrow=moving_sample(AK_ARROW,hit);wideArrow.a={100,150};wideArrow.b={300,150};wideArrow.width=30;
        PointF arrowTip{300-108*cosf(.42f),150-108*sinf(.42f)};
        report(wideArrow.hit_test(arrowTip,SF(3))&&wideArrow.bounds().Contains(arrowTip),
            "wide arrowhead hit and dirty bounds include the full triangle beyond shaft bounding box");
        Ann number=moving_sample(AK_NUMBER,hit);
        std::vector<Ann> stack(3,number);
        for(int i=0;i<3;++i){stack[i].index=i+1;stack[i].color=Color(255,40+i*70,160-i*30,60);}
        prepare(stack);o.tool=T_PEN;POINT point=client(number.pos);
        auto click=[&](UINT press=WM_RBUTTONDOWN){
            send(press,point,MK_RBUTTON);send(WM_RBUTTONUP,point);
        };
        click();bool cycle=o.selectedAnnotation==2;
        click(WM_RBUTTONDBLCLK);cycle=cycle&&o.selectedAnnotation==1;
        click();cycle=cycle&&o.selectedAnnotation==0;
        click(WM_RBUTTONDBLCLK);cycle=cycle&&o.selectedAnnotation==2;
        report(cycle&&o.undoStack.empty()&&o.anns[0].index==1&&o.anns[2].index==3,
            "repeated right clicks cycle top middle bottom and wrap without changing layer order or history");
        click();report(o.selectedAnnotation==1,"right click can select lower object before dragging");
        send(WM_RBUTTONDOWN,point,MK_RBUTTON);
        send(WM_MOUSEMOVE,{point.x+40,point.y+20},MK_RBUTTON);
        send(WM_RBUTTONUP,{point.x+40,point.y+20});
        report(o.anns[1].pos.X==number.pos.X+40&&o.anns[0].pos.X==number.pos.X&&
            o.anns[2].pos.X==number.pos.X&&o.selectedAnnotation==1,
            "dragging selected lower object is not hijacked by overlapping upper object");
        RECT unchanged=o.sel;
        SendMessageW(form,WM_KEYDOWN,VK_RIGHT,0);
        report(o.anns[1].pos.X==number.pos.X+41&&EqualRect(&unchanged,&o.sel),
            "arrow key nudges selected object instead of moving entire screenshot selection");
        undo(&o);select_annotation(&o,1);
        std::unique_ptr<Bitmap> preview(render_result(&o));
        {Graphics g(preview.get());g.SetSmoothingMode(SmoothingModeAntiAlias);draw_annotation_selection(g,&o);}
        report(save_bitmap_file(preview.get(),(output/(scale==1?L"annotation-moving-100.png":L"annotation-moving-175.png")).wstring(),92),
            "selected overlapping annotation and right-button hint snapshot rendered");
        for(UINT cancel:{(UINT)WM_KEYDOWN,(UINT)WM_KILLFOCUS,(UINT)WM_CANCELMODE}){
            prepare({number});send(WM_RBUTTONDOWN,point,MK_RBUTTON);
            send(WM_MOUSEMOVE,{point.x+50,point.y+30},MK_RBUTTON);
            SendMessageW(form,cancel,cancel==WM_KEYDOWN?VK_ESCAPE:0,0);
            send(WM_RBUTTONUP,{point.x+50,point.y+30});
            report(!o.annotationDrag.active&&!o.sizeDrag.active&&o.anns[0].pos.X==number.pos.X&&
                o.anns[0].pos.Y==number.pos.Y&&o.undoStack.empty()&&GetCapture()!=form&&o.alive,
                "Escape focus loss and cancel mode restore original position and consume trailing release");
        }
        prepare({number});send(WM_RBUTTONDOWN,point,MK_RBUTTON);
        send(WM_MOUSEMOVE,{point.x+50,point.y+30},MK_RBUTTON);
        HWND other=CreateWindowW(L"STATIC",L"move-capture-owner",WS_POPUP,-5000,-3000,10,10,nullptr,nullptr,g_hinst,nullptr);
        SetCapture(other);
        report(!o.annotationDrag.active&&o.anns[0].pos.X==number.pos.X&&GetCapture()==other,
            "capture transfer rolls back movement without releasing another window capture");
        ReleaseCapture();DestroyWindow(other);
        prepare({number});send(WM_RBUTTONDOWN,point,MK_RBUTTON);
        send(WM_MOUSEMOVE,{point.x+40,point.y+20},MK_RBUTTON);
        send(WM_MOUSEMOVE,{point.x+40,point.y+20});
        report(!o.annotationDrag.active&&o.anns[0].pos.X==number.pos.X+40&&o.undoStack.size()==1&&GetCapture()!=form,
            "missing right-button release is recovered without leaving drag active");
        prepare({number});send(WM_RBUTTONDOWN,point,MK_RBUTTON);
        send(WM_MOUSEMOVE,{-60,-40},MK_RBUTTON);send(WM_RBUTTONUP,{-60,-40});
        report(!o.annotationDrag.active&&o.undoStack.size()==1&&GetCapture()!=form,
            "pointer capture keeps drag and release working outside screenshot window");
        prepare({number});send(WM_LBUTTONDOWN,point,MK_LBUTTON);
        send(WM_MOUSEMOVE,{point.x+30,point.y+30},MK_LBUTTON);send(WM_LBUTTONUP,{point.x+30,point.y+30});
        report(o.anns.size()==2&&o.anns[0].pos.X==number.pos.X&&o.anns.back().kind==AK_RECT,
            "left drawing over existing shape creates new object instead of moving old one");
        prepare({number});o.tool=T_NUMBER;send(WM_LBUTTONDOWN,point,MK_LBUTTON);send(WM_LBUTTONUP,point);
        send(WM_LBUTTONDBLCLK,point,MK_LBUTTON);send(WM_LBUTTONUP,point);
        report(o.anns.size()==3&&o.alive,"rapid double click stamps twice instead of copying and closing screenshot");
        PointF textHit;Ann text=moving_sample(AK_TEXT,textHit);prepare({text});o.tool=T_TEXT;
        POINT body=client(textHit);
        send(WM_LBUTTONDOWN,body,MK_LBUTTON);
        report(o.editing&&o.editingIndex==-1&&same_text(o.anns[0],text),
            "left text placement over existing text opens independent new draft");
        send(WM_LBUTTONUP,body);send(WM_LBUTTONDBLCLK,body,MK_LBUTTON);
        report(o.editing&&o.editingIndex==0&&o.editText==text.text&&o.anns.size()==1,
            "double click reopens existing text after normal first-click draft without duplicate text");
        commit_text(&o);
        prepare({text});o.tool=T_TEXT;begin_text(&o,{},0);
        send(WM_RBUTTONDOWN,body,MK_RBUTTON);
        send(WM_MOUSEMOVE,{body.x+25,body.y+15},MK_RBUTTON);send(WM_RBUTTONUP,{body.x+25,body.y+15});
        report(!o.editing&&o.selectedAnnotation==0&&o.selectedText==-1&&o.anns.size()==1&&
            o.anns[0].text==text.text&&fabs(o.anns[0].pos.X-text.pos.X-25)<.01f,
            "right moving edited text finishes in movement selection instead of reopening resize controls");
        commit_text(&o);
        Ann underEmpty=number;underEmpty.pos=textHit;prepare({text,underEmpty});o.tool=T_TEXT;
        begin_text(&o,{},0);o.editText.clear();send(WM_RBUTTONDOWN,body,MK_RBUTTON);
        send(WM_MOUSEMOVE,{body.x+25,body.y+15},MK_RBUTTON);send(WM_RBUTTONUP,{body.x+25,body.y+15});
        report(!o.editing&&o.anns.size()==1&&o.anns[0].kind==AK_NUMBER&&
            fabs(o.anns[0].pos.X-underEmpty.pos.X-25)<.01f,
            "deleting empty edited text before right drag never resumes editing shifted underlying object");
        Ann mosaic=moving_sample(AK_MOSAIC,hit);prepare({mosaic});delete o.mosaic;o.mosaic=nullptr;
        POINT mosaicPoint=client(hit);send(WM_RBUTTONDOWN,mosaicPoint,MK_RBUTTON);
        report(o.annotationDrag.active&&o.mosaic,"moving mosaic restores source sampling cache after selection changes");
        SendMessageW(form,WM_KEYDOWN,VK_ESCAPE,0);send(WM_RBUTTONUP,mosaicPoint);
        prepare({number});o.tool=T_NUMBER;send(WM_RBUTTONDOWN,point,MK_RBUTTON);
        send(WM_RBUTTONUP,point);send(WM_KEYDOWN,{},VK_ESCAPE);
        report(o.selectedAnnotation==-1&&o.alive,"Escape clears selected annotation before closing screenshot");
        prepare({number});o.tool=T_PEN;
        send(WM_RBUTTONDOWN,{900,600},MK_RBUTTON);
        report(o.sizeDrag.active&&!o.annotationDrag.active,"empty canvas retains right-drag tool sizing");
        send(WM_RBUTTONUP,{930,600});
        o.tbVisible=true;o.tbX=10;o.tbY=800;POINT button{};
        for(const auto& item:o.items)if(item.id==T_PEN)
            button={(LONG)(o.tbX+item.rect.X+item.rect.Width/2),(LONG)(o.tbY+item.rect.Y+item.rect.Height/2)};
        Ann underToolbar=number;underToolbar.pos={(REAL)(button.x-o.sel.left),(REAL)(button.y-o.sel.top)};
        o.anns={underToolbar};send(WM_RBUTTONDOWN,button,MK_RBUTTON);
        report(o.sizeDrag.active&&!o.annotationDrag.active,"toolbar right sizing has priority over overlapping annotation");
        send(WM_RBUTTONUP,button);o.tbVisible=false;
        prepare({number});o.panel=PM_TEXT;o.panelRect={(REAL)point.x-30,(REAL)point.y-30,100,100};
        send(WM_RBUTTONDOWN,point,MK_RBUTTON);
        report(!o.annotationDrag.active&&!o.sizeDrag.active,"result panel shields underlying objects from right drag");
        o.panel=PM_HIDDEN;prepare({number});send(WM_RBUTTONDOWN,point,MK_RBUTTON);
        send(WM_MOUSEMOVE,{point.x+40,point.y+20},MK_RBUTTON);
        DestroyWindow(form);
        report(!o.annotationDrag.active&&GetCapture()!=form&&!o.alive,
            "closing screenshot during right drag releases capture and cancels pending movement");
        stampTestForeground=nullptr;
    }
    g_scale=originalScale;
    std::cout<<"Annotation moving checks: "<<checks<<std::endl;return passed;
}
