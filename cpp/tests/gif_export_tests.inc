static bool test_gif_export(const std::filesystem::path& output){
    LogTrace trace;double originalScale=g_scale;g_scale=1;
    int checks=0;bool passed=true;
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;
        logf(L"动态 GIF 回归：%s 成功=%d",u8_to_w(name).c_str(),(int)ok);
    };
    auto fixtures=std::filesystem::path(__FILE__).parent_path()/L"fixtures"/L"gif";
    auto dir=output/L"gif-export";std::filesystem::create_directories(dir/L"images");
    {std::ofstream info(dir/L"trace.txt");info<<"shot-"<<GetCurrentProcessId()<<"-"<<trace.id();}
    for(const auto* name:{L"short.gif",L"long.gif",L"disposal.gif"})
        std::filesystem::copy_file(fixtures/name,dir/L"images"/name,std::filesystem::copy_options::overwrite_existing);
    imageTestExe=(dir/L"fixture.exe").wstring();emoji_reload();
    auto load=[&](const wchar_t* name){
        for(int i=0;i<emoji_count();++i)if(!wcscmp(emoji_text(i),name))return emoji_animation(i);
        return std::shared_ptr<StampAnimation>();
    };
    auto shorter=load(L"short.gif"),longer=load(L"long.gif"),disposal=load(L"disposal.gif");
    report(shorter&&shorter->frames.size()==2&&shorter->duration==200&&shorter->delays[0]==70&&shorter->delays[1]==130,
           "short GIF retains both frames and native variable delays");
    report(longer&&longer->frames.size()==3&&longer->duration==550,
           "long GIF retains longest duration");
    bool disposed=disposal&&disposal->frames.size()==4;
    if(disposed)for(size_t i=0;i<disposal->frames.size();++i){
        Bitmap reference((fixtures/(L"disposal-"+std::to_wstring(i)+L".png")).c_str());
        disposed=disposed&&same_stamp_pixels(disposal->frames[i].get(),&reference);
    }
    report(disposed,"WIC transparent offset disposal 2 and disposal 3 match independent reference frames");
    if(!shorter||!longer||!disposal){imageTestExe.clear();emoji_reload();g_scale=originalScale;return false;}
    report(shorter->frame_index(0)==0&&shorter->frame_index(69)==0&&shorter->frame_index(70)==1&&
           shorter->frame_index(200)==0&&shorter->frame_index(470)==1,
           "short animation loops while preserving variable frame intervals");
    report(shorter->next_change(0)==70&&shorter->next_change(70)==200&&shorter->next_change(270)==400,
           "animation scheduler uses exact next source-frame boundary");
    auto scene=std::make_shared<AnimatedScreenshot>();scene->scale=1;
    scene->background=std::make_shared<Bitmap>(960,640,PixelFormat32bppRGB);
    {
        Graphics g(scene->background.get());g.Clear(Color::White);
        for(int y=0;y<640;y+=16)for(int x=0;x<960;x+=16){
            SolidBrush ink(Color(255,(BYTE)(40+(x/16%4)*55),(BYTE)(60+(y/16%4)*40),(BYTE)(80+(x/16%2)*80)));
            g.FillRectangle(&ink,x,y,8,8);
        }
    }
    Ann a;a.kind=AK_EMOJI;a.emojiAnimation=shorter;a.emojiImage=shorter->frames[0];a.fontSize=72;a.pos={140,140};
    Ann b=a;b.emojiAnimation=longer;b.emojiImage=longer->frames[0];b.fontSize=96;b.pos={360,200};
    Ann top;top.kind=AK_RECT;top.rect={335,185,20,20};top.color=Color::Black;top.width=4;
    scene->annotations={a,b,top};
    report(scene->duration()==550&&scene->next_change(0)==70&&scene->next_change(70)==90,
           "mixed timeline follows longest GIF and union of frame boundaries");
    auto first=scene->render(0),later=scene->render(90);
    report(first&&later&&!same_stamp_pixels(first.get(),later.get()),
           "animated screenshot renders distinct composite frames with annotation order");
    RECT region{100,90,420,260};auto cropped=scene->render(90,&region);
    std::unique_ptr<Bitmap> reference(later->Clone(Rect(100,90,320,170),PixelFormat32bppRGB));
    report(cropped&&same_stamp_pixels(cropped.get(),reference.get()),
           "visible-region rendering preserves composition without full longshot redraw");
    save_bitmap_file(first.get(),(dir/L"first.png").wstring(),92);
    bool exported=save_animated_gif(*scene,(dir/L"mixed.gif").wstring());
    report(exported&&std::filesystem::file_size(dir/L"mixed.gif")>0,"optimized animated GIF is written");
    ComPtr<IWICImagingFactory> factory;CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr=factory->CreateDecoderFromFilename((dir/L"mixed.gif").c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
    UINT count=0;if(SUCCEEDED(hr))decoder->GetFrameCount(&count);
    uint64_t delay=0,area=0;bool patches=true,transparentPatches=true;std::vector<UINT> delays;
    for(UINT i=0;i<count;++i){
        ComPtr<IWICBitmapFrameDecode> frame;decoder->GetFrame(i,&frame);
        ComPtr<IWICMetadataQueryReader> info;frame->GetMetadataQueryReader(&info);
        UINT w=0,h=0;frame->GetSize(&w,&h);area+=(uint64_t)w*h;
        UINT duration=gif_metadata(info.Get(),L"/grctlext/Delay")*10;
        delays.push_back(duration);delay+=duration;
        if(i){
            patches=patches&&w<=148&&h<=132;
            transparentPatches=transparentPatches&&gif_metadata(info.Get(),L"/grctlext/TransparencyFlag")!=0;
        }
    }
    report(count>=8&&delay==550,"encoded frames total exactly longest duration without time truncation");
    report(patches&&area<(uint64_t)960*640*count/3,"subsequent frames contain cropped changes instead of full static background");
    report(transparentPatches,"unchanged pixels inside delta rectangles use transparent palette index");
    decoder.Reset();
    std::ofstream timeline(dir/L"timeline.txt");
    uint64_t time=0;size_t expectedCount=0;
    while(time<scene->duration()){
        auto frame=scene->render(time);
        save_bitmap_file(frame.get(),(dir/(L"expected-"+std::to_wstring(time)+L".png")).wstring(),92);
        uint64_t next=scene->next_change(time);
        timeline<<time<<" "<<next-time<<"\n";time=next;++expectedCount;
    }
    timeline.close();
    report(count==expectedCount,"encoder retains all meaningful mixed-animation transitions");
    auto settingsBefore=load_settings(),settings=settingsBefore;
    settings.save_dir=dir.wstring();settings.auto_copy=false;settings.image_format=L"png";save_settings(settings);
    auto savedAnimationPrompts=animationSavePrompts;
    std::wstring path;bool cancelled=false;
    animationSaveDecision=IDYES;
    report(save_bitmap_direct(first.get(),dir.wstring(),L"png",92,path,nullptr,scene,&cancelled)&&
           std::filesystem::path(path).extension()==L".gif"&&!cancelled,
           "Save Yes overrides static settings with animated GIF");
    animationSaveDecision=IDNO;
    report(save_bitmap_direct(later.get(),dir.wstring(),L"png",92,path,nullptr,scene,&cancelled)&&
           std::filesystem::path(path).extension()==L".png",
           "Save No keeps configured static format");
    Bitmap staticResult(path.c_str());
    report(same_stamp_pixels(&staticResult,first.get()),"static save uses every GIF first frame rather than current preview frame");
    animationSaveDecision=IDCANCEL;
    report(!save_bitmap_direct(first.get(),dir.wstring(),L"png",92,path,nullptr,scene,&cancelled)&&
           cancelled&&path.empty(),"cancelled animation prompt writes no file");
    animationSaveDecision=IDYES;fileDialogCancelled=false;fileDialogFilter=1;
    fileDialogTarget=(dir/L"save-as-wrong.png").wstring();
    report(save_bitmap_dialog(nullptr,first.get(),dir.wstring(),L"jpg",92,&path,scene,&cancelled)&&
           std::filesystem::path(path).extension()==L".gif"&&std::filesystem::exists(dir/L"save-as-wrong.gif"),
           "Save As Yes enforces GIF extension even when filename names PNG");
    animationSaveDecision=IDNO;fileDialogTarget=(dir/L"save-as-static.png").wstring();
    report(save_bitmap_dialog(nullptr,later.get(),dir.wstring(),L"png",92,&path,scene,&cancelled)&&
           std::filesystem::path(path).extension()==L".png","Save As No keeps static encoder selection");
    animationSaveDecision=IDYES;fileDialogCancelled=true;
    report(!save_bitmap_dialog(nullptr,first.get(),dir.wstring(),L"png",92,&path,scene,&cancelled)&&
           cancelled,"cancelled GIF file chooser retains editing state");
    fileDialogCancelled=false;
    auto noAnimation=std::make_shared<AnimatedScreenshot>();noAnimation->background=scene->background;
    int promptCount=animationSavePrompts;
    report(save_bitmap_direct(first.get(),dir.wstring(),L"png",92,path,nullptr,noAnimation)&&
           animationSavePrompts==promptCount,"static-only screenshot does not prompt for animation");
    HWND preview=create_longshot_preview(first->Clone(0,0,960,640,PixelFormat32bppRGB),L"GIF mixed preview");
    auto* previewData=(Preview*)GetWindowLongPtrW(preview,GWLP_USERDATA);
    previewData->animation=scene;
    promptCount=animationSavePrompts;SendMessageW(preview,WM_COMMAND,1,0);
    report(animationSavePrompts==promptCount+1&&IsWindow(preview),"longshot Save asks about preserving dynamic stickers");
    fileDialogTarget=(dir/L"longshot-as.gif").wstring();promptCount=animationSavePrompts;
    SendMessageW(preview,WM_COMMAND,4,0);
    report(animationSavePrompts==promptCount+1&&std::filesystem::exists(dir/L"longshot-as.gif"),
           "longshot Save As uses same animated save flow");
    DestroyWindow(preview);
    int pinsBefore=pin_count();
    pin_create(first->Clone(0,0,960,640,PixelFormat32bppRGB),-4000,-3000,scene);
    auto* pinned=g_pins.back();
    fileDialogTarget=(dir/L"pinned-as.gif").wstring();promptCount=animationSavePrompts;
    pin_save_as(pinned);
    report(animationSavePrompts==promptCount+1&&std::filesystem::exists(dir/L"pinned-as.gif"),
           "pinned screenshot Save As retains animations and asks before export");
    pinned->animationStart=GetTickCount64()-300;SendMessageW(pinned->hwnd,WM_TIMER,1,0);
    report(pinned->animationFrame!=nullptr,"pinned GIF preview advances without re-rendering unchanged frames");
    pin_remove(pinned);
    report(pin_count()==pinsBefore,"closing pinned animation releases its window and retained document");
    {
        Overlay overlay;overlay.traceId=trace.id();overlay.hasSel=true;overlay.sel={0,0,960,640};
        overlay.shot.vw=960;overlay.shot.vh=640;
        overlay.shot.bmp=scene->background->Clone(0,0,960,640,PixelFormat32bppRGB);
        overlay.anns=scene->annotations;
        HWND form=CreateWindowW(OV_CLASS,L"animated-save-entry-test",WS_POPUP,
            -4000,-3000,960,640,nullptr,nullptr,g_hinst,&overlay);
        promptCount=animationSavePrompts;
        fileDialogTarget=(dir/L"overlay-as.gif").wstring();do_save(&overlay,true);
        report(animationSavePrompts==promptCount+1&&std::filesystem::exists(dir/L"overlay-as.gif"),
               "screenshot Save As carries dynamic layers into shared output");
        animationSaveDecision=IDCANCEL;promptCount=animationSavePrompts;
        do_save(&overlay);
        report(animationSavePrompts==promptCount+1&&IsWindow(form)&&overlay.alive,
               "screenshot Save Cancel keeps editing state and dynamic layers");
        DestroyWindow(form);
    }
    {
        auto identical=std::make_shared<StampAnimation>();
        identical->frames={shorter->frames[0],shorter->frames[0]};
        identical->delays={100,100};identical->duration=200;
        AnimatedScreenshot still=*scene;still.annotations={a};
        still.annotations[0].emojiAnimation=identical;
        bool ok=save_animated_gif(still,(dir/L"merged-identical.gif").wstring());
        ComPtr<IWICBitmapDecoder> merged;
        if(ok)ok=SUCCEEDED(factory->CreateDecoderFromFilename((dir/L"merged-identical.gif").c_str(),nullptr,
            GENERIC_READ,WICDecodeMetadataCacheOnLoad,&merged));
        UINT frames=0;if(ok)merged->GetFrameCount(&frames);
        ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICMetadataQueryReader> info;
        if(frames==1){merged->GetFrame(0,&frame);frame->GetMetadataQueryReader(&info);}
        report(ok&&frames==1&&gif_metadata(info.Get(),L"/grctlext/Delay")==20,
               "identical animation frames merge into a single compressed background with accumulated delay");
        merged.Reset();frame.Reset();info.Reset();
        identical->delays={350000,350000};identical->duration=700000;
        ok=save_animated_gif(still,(dir/L"long-hold.gif").wstring());
        if(ok)ok=SUCCEEDED(factory->CreateDecoderFromFilename((dir/L"long-hold.gif").c_str(),nullptr,
            GENERIC_READ,WICDecodeMetadataCacheOnLoad,&merged));
        frames=0;if(ok)merged->GetFrameCount(&frames);
        uint64_t duration=0;UINT lastWidth=0,lastHeight=0;
        for(UINT i=0;i<frames;++i){
            frame.Reset();info.Reset();merged->GetFrame(i,&frame);frame->GetMetadataQueryReader(&info);
            duration+=gif_metadata(info.Get(),L"/grctlext/Delay")*10;
            frame->GetSize(&lastWidth,&lastHeight);
        }
        report(ok&&frames==2&&duration==700000&&lastWidth==1&&lastHeight==1,
               "holds beyond GIF delay field use one-pixel continuation instead of repeating static background");
    }
    auto invalid=std::make_shared<AnimatedScreenshot>(*scene);
    invalid->background=std::make_shared<Bitmap>(65536,2,PixelFormat32bppRGB);
    report(!save_animated_gif(*invalid,(dir/L"too-wide.gif").wstring())&&!std::filesystem::exists(dir/L"too-wide.gif"),
           "GIF dimension limit reports failure without writing truncated output");
    report(animationSavePrompts>savedAnimationPrompts,"dynamic save decisions are exercised through shared output entry points");
    save_settings(settingsBefore);imageTestExe.clear();emoji_reload();g_scale=originalScale;
    std::cout<<"Animated GIF checks: "<<checks<<std::endl;return passed;
}
