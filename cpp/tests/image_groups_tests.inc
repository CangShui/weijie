static bool test_image_groups(const std::filesystem::path& output){
    LogTrace trace;double originalScale=g_scale;bool passed=true;int checks=0;
    auto report=[&](bool ok,const char* name){
        ++checks;passed=passed&&ok;
        std::cout<<(ok?"PASS ":"FAIL ")<<name<<" scale="<<g_scale<<std::endl;
        logf(L"图片分组回归：%s DPI=%.2f 成功=%d",u8_to_w(name).c_str(),g_scale,(int)ok);
    };
    auto fixtures=std::filesystem::path(__FILE__).parent_path()/L"fixtures"/L"gif";
    for(double scale:{1.0,1.75}){
        g_scale=scale;
        auto root=output/L"isolated"/(L"groups-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring((int)(scale*100)));
        for(const auto* folder:{L"default",L"游戏",L"游戏/庆祝",L"空分组"})
            std::filesystem::create_directories(root/L"images"/folder);
        imageTestExe=(root/L"fixture.exe").wstring();
        auto picture=[&](const wchar_t* file,Color color){
            Bitmap image(40,24,PixelFormat32bppARGB);
            {Graphics g(&image);g.Clear(color);}
            return save_bitmap_file(&image,(root/L"images"/file).wstring(),92);
        };
        bool made=picture(L"loose.png",Color(255,240,155,40))&&
            picture(L"default/a.png",Color(255,220,55,65))&&picture(L"default/b.png",Color(255,85,195,125))&&
            picture(L"游戏/a.png",Color(255,45,125,240))&&picture(L"游戏/b.png",Color(255,165,75,225))&&
            picture(L"游戏/庆祝/star.png",Color(255,235,195,45));
        std::filesystem::copy_file(fixtures/L"short.gif",root/L"images"/L"游戏"/L"animation.gif",
            std::filesystem::copy_options::overwrite_existing);
        report(made&&emoji_reload()==7&&emoji_groups().size()==5,"folders and unclassified root pictures form five independent groups");
        auto find=[&](const wchar_t* key)->const StampGroup*{
            for(const auto& group:emoji_groups())if(group.key==key)return &group;
            return nullptr;
        };
        auto* defaults=find(L"default");auto* rootGroup=find(L"");auto* custom=find(L"游戏");
        report(defaults&&defaults->name==L"默认"&&defaults->images.size()==2&&rootGroup&&
               rootGroup->name==L"未分类"&&rootGroup->images.size()==1&&
               custom&&custom->name==L"游戏"&&custom->images.size()==3,
               "default is labeled Default and only root pictures are Unclassified");
        report(find(L"游戏/庆祝")&&find(L"游戏/庆祝")->images.size()==1&&
               find(L"空分组")&&find(L"空分组")->images.empty(),
               "nested and empty user folders are represented without merging image names");
        report(emoji_groups().front().key==L"default"&&emoji_groups().back().key.empty(),
               "default appears first and Unclassified appears last");
        int retainedIndex=defaults?defaults->images.front():-1;
        auto retained=emoji_image(retainedIndex);
        Overlay overlay;overlay.traceId=trace.id();overlay.hasSel=true;overlay.sel={0,0,850,850};
        overlay.shot.vx=-4000;overlay.shot.vy=-3000;overlay.shot.vw=900;overlay.shot.vh=1200;
        overlay.tbVisible=true;overlay.tbX=0;overlay.tbY=1000;
        HWND form=CreateWindowW(OV_CLASS,L"image-groups-test",WS_POPUP,
            -4000,-3000,900,1200,nullptr,nullptr,g_hinst,&overlay);
        if(!form){passed=false;continue;}
        stampTestForeground=form;show_emoji_picker(&overlay);
        auto* popup=(EmojiPopup*)GetWindowLongPtrW(overlay.emojiPopup,GWLP_USERDATA);
        report(popup&&std::count_if(popup->display.begin(),popup->display.end(),
            [](const auto& row){return row.header;})==5&&popup->count==7,
            "picker builds a titled section for every discovered folder");
        HWND picker=overlay.emojiPopup;
        SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(S(25),S(45)));
        report(overlay.emojiPopup==picker&&overlay.anns.empty(),"group headers cannot place or select a picture");
        report(png(picker,(output/(scale==1?L"image-groups-100.png":L"image-groups-175.png")).wstring()),
               "group headers and image grids render in native picker");
        int animationIndex=-1;
        for(int index:find(L"游戏")->images)if(!wcscmp(emoji_text(index),L"animation.gif"))animationIndex=index;
        POINT location=image_picker_point(picker,animationIndex);
        SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(location.x,location.y));
        report(!overlay.emojiPopup&&overlay.emojiGroup==L"游戏"&&overlay.emojiAnimation&&
               overlay.emojiAnimation->duration==200,"GIF remains selectable and animated inside custom folder group");
        show_emoji_picker(&overlay);picker=overlay.emojiPopup;
        auto tick=[&](){
            ULONGLONG until=GetTickCount64()+650;
            do{SendMessageW(picker,WM_TIMER,1,0);Sleep(25);}while(GetTickCount64()<until);
        };
        std::filesystem::remove(root/L"images"/L"default"/L"a.png");tick();
        popup=(EmojiPopup*)GetWindowLongPtrW(picker,GWLP_USERDATA);
        report(popup->count==6&&find(L"default")&&find(L"default")->images.size()==1,
               "deleting default picture refreshes the open picker without restarting");
        std::filesystem::remove(root/L"images"/L"default"/L"b.png");tick();
        report(find(L"default")&&find(L"default")->images.empty(),"empty existing default folder retains its empty group");
        std::filesystem::remove(root/L"images"/L"default");tick();
        report(!find(L"default")&&emoji_groups().size()==4&&!std::filesystem::exists(root/L"images"/L"default"),
               "deleting default directory removes its entire section without recreating it");
        Color retainedPixel;if(retained)retained->GetPixel(0,0,&retainedPixel);
        report(retained&&retainedPixel.GetR()==220&&retainedPixel.GetG()==55,
               "existing screenshot image references survive source file and folder deletion");
        for(const auto* file:{L"游戏/a.png",L"游戏/b.png",L"游戏/animation.gif",L"游戏/庆祝/star.png"})
            std::filesystem::remove(root/L"images"/file);
        std::filesystem::remove(root/L"images"/L"游戏/庆祝");std::filesystem::remove(root/L"images"/L"游戏");
        tick();
        report(!find(L"游戏")&&!find(L"游戏/庆祝")&&find(L"")&&emoji_count()==1,
               "removing custom folder and its nested folder clears both sections and retains loose image");
        std::filesystem::create_directories(root/L"images"/L"新增");
        picture(L"新增/new.png",Color(255,40,180,150));tick();
        report(find(L"新增")&&find(L"新增")->images.size()==1&&emoji_count()==2,
               "new user folder and picture appear live in existing picker");
        std::filesystem::remove(root/L"images"/L"loose.png");tick();
        report(!find(L"")&&find(L"新增"),"Unclassified section disappears when last root image is deleted");
        close_emoji_popup(&overlay);
        report(!overlay.emojiPopup,"closing grouped picker releases notification window");
        DestroyWindow(form);stampTestForeground=nullptr;
    }
    imageTestExe.clear();emoji_reload();g_scale=originalScale;
    std::cout<<"Image group checks: "<<checks<<std::endl;return passed;
}
