static void record_annotation_change(Overlay* o,int index,std::optional<Ann> before,std::optional<Ann> after){
    clear_annotation_selection(o,L"标注内容或位置已改变");
    o->undoStack.push_back({index,std::move(before),std::move(after)});o->redoStack.clear();
    logf(L"标注历史已记录：索引=%d 操作=%s 可撤销步数=%zu",index,
        !o->undoStack.back().before?L"新增":!o->undoStack.back().after?L"删除":L"修改",o->undoStack.size());
}
static void add_annotation(Overlay* o,const Ann& annotation){
    int index=(int)o->anns.size();o->anns.push_back(annotation);
    record_annotation_change(o,index,std::nullopt,annotation);
}
static Ann editing_text(const Overlay* o){
    Ann draft=o->textDraft;draft.kind=AK_TEXT;draft.pos=o->editPos;
    draft.text=o->editText+o->composing;
    draft.fontSize=(float)o->toolSizes[T_TEXT-T_RECT];draft.color=o->ink;return draft;
}
static bool same_text(const Ann& a,const Ann& b){
    return a.text==b.text&&a.color.GetValue()==b.color.GetValue()&&a.fontSize==b.fontSize&&
        fabs(a.pos.X-b.pos.X)<.001f&&fabs(a.pos.Y-b.pos.Y)<.001f&&
        fabs(a.textScaleX-b.textScaleX)<.001f&&fabs(a.textScaleY-b.textScaleY)<.001f&&fabs(a.textAngle-b.textAngle)<.001f;
}
static bool text_interaction_at(Overlay* o,POINT pt){
    if(!o->hasSel||o->mode!=0||o->sizeDrag.active||(o->tool!=T_NONE&&o->tool!=T_TEXT))return false;
    if(o->tbVisible&&ptInRectF(RectF(o->tbX,o->tbY,toolbar_width(o),SF(TB_HEIGHT)),(float)pt.x,(float)pt.y))return false;
    return o->panel==PM_HIDDEN||!ptInRectF(o->panelRect,(float)pt.x,(float)pt.y);
}
static int text_at(Overlay* o,POINT pt){
    if(!text_interaction_at(o,pt)||!ptInRect(o->sel,pt.x,pt.y))return -1;
    PointF point((REAL)(pt.x-o->sel.left),(REAL)(pt.y-o->sel.top));
    for(int i=(int)o->anns.size()-1;i>=0;--i)
        if((!o->editing||i!=o->editingIndex)&&o->anns[i].kind==AK_TEXT&&o->anns[i].text_contains(point))return i;
    return -1;
}
static PointF text_handle(const Ann& text,int handle){
    auto size=text.text_frame_size();
    const float x[]={0,.5f,1,1,1,.5f,0,0,.5f};
    const float y[]={0,0,0,.5f,1,1,1,.5f,0};
    float pad=SF(6);
    return text.text_point({(size.Width+pad*2)*x[handle]-pad,
        handle==8?-pad-SF(24):(size.Height+pad*2)*y[handle]-pad});
}
static bool selected_text(Overlay* o,Ann& text){
    if(o->editing){text=editing_text(o);return true;}
    if(o->selectedText>=0&&o->selectedText<(int)o->anns.size()&&o->anns[o->selectedText].kind==AK_TEXT){
        text=o->anns[o->selectedText];return true;
    }
    return false;
}
static int text_frame_hit(Overlay* o,POINT pt){
    if(o->annotationDrag.active||o->selectedAnnotation>=0)return -1;
    if(!text_interaction_at(o,pt))return -1;
    Ann text;if(!selected_text(o,text))return -1;
    PointF point((REAL)(pt.x-o->sel.left),(REAL)(pt.y-o->sel.top));
    for(int i=8;i>=0;--i){
        auto handle=text_handle(text,i);float x=point.X-handle.X,y=point.Y-handle.Y;
        if(x*x+y*y<=SF(7)*SF(7))return i;
    }
    auto local=text.text_local(point);auto size=text.text_frame_size();float pad=SF(6),hit=SF(3);
    if(local.X>=-pad-hit&&local.Y>=-pad-hit&&local.X<=size.Width+pad+hit&&local.Y<=size.Height+pad+hit&&
       (fabs(local.X+pad)<hit||fabs(local.Y+pad)<hit||fabs(local.X-size.Width-pad)<hit||fabs(local.Y-size.Height-pad)<hit))return 9;
    return -1;
}
static LPCWSTR text_handle_cursor(const Ann& text,int handle){
    if(handle==8)return IDC_HAND;
    if(handle==9)return IDC_SIZEALL;
    float base=handle==0||handle==4?45:handle==2||handle==6?135:handle==1||handle==5?90:0;
    int direction=((int)lround((base+text.textAngle)/45)%4+4)%4;
    const wchar_t* cursors[]={IDC_SIZEWE,IDC_SIZENWSE,IDC_SIZENS,IDC_SIZENESW};return cursors[direction];
}
static void invalidate_text_frame(Overlay* o,const Ann& text){
    auto r=text.bounds();float pad=SF(36);
    RECT dirty{(LONG)floor(r.X-pad)+o->sel.left,(LONG)floor(r.Y-pad)+o->sel.top,
        (LONG)ceil(r.GetRight()+pad)+o->sel.left,(LONG)ceil(r.GetBottom()+pad)+o->sel.top};
    InvalidateRect(o->hwnd,&dirty,FALSE);
}
static void draw_text_frame(Graphics& g,Overlay* o){
    if(o->annotationDrag.active||o->selectedAnnotation>=0)return;
    if(o->tool!=T_NONE&&o->tool!=T_TEXT)return;
    Ann text;if(!selected_text(o,text))return;
    PointF corners[]={text_handle(text,0),text_handle(text,2),text_handle(text,4),text_handle(text,6)};
    Pen edge(Color(255,7,193,96),SF(1));edge.SetDashStyle(DashStyleDash);
    g.DrawPolygon(&edge,corners,4);
    auto top=text_handle(text,1),rotate=text_handle(text,8);
    Pen solid(Color(255,7,193,96),SF(1));g.DrawLine(&solid,top,rotate);
    SolidBrush white(Color::White);
    for(int i=0;i<9;++i){
        auto point=text_handle(text,i);float radius=SF(i==8?5.0f:3.5f);
        g.FillEllipse(&white,point.X-radius,point.Y-radius,radius*2,radius*2);
        g.DrawEllipse(&solid,point.X-radius,point.Y-radius,radius*2,radius*2);
    }
    g.DrawArc(&solid,rotate.X-SF(3),rotate.Y-SF(3),SF(6),SF(6),30.0f,260.0f);
    if(o->textDrag.kind==3){
        auto angle=std::to_wstring((int)lround(text.textAngle))+L"°";
        ui::text(g,angle,RectF(rotate.X+SF(10),rotate.Y-SF(10),SF(65),SF(24)),11,Color(255,7,125,75),true);
    }
}
static void begin_text(Overlay* o,POINT pt,int index=-1){
    commit_text(o);clear_annotation_selection(o,L"进入文字内容编辑");
    o->editing=true;o->editingIndex=index;o->selectedText=index;
    o->editText.clear();o->composing.clear();o->textDraft=Ann();o->textDraft.kind=AK_TEXT;
    if(index>=0){
        o->textDraft=o->anns[index];o->editText=o->textDraft.text;
        o->toolSizes[T_TEXT-T_RECT]=(int)o->textDraft.fontSize;o->ink=o->textDraft.color;
    }else{
        o->textDraft.pos={(REAL)(pt.x-o->sel.left),(REAL)(pt.y-o->sel.top)};
        o->textDraft.fontSize=(float)o->toolSizes[T_TEXT-T_RECT];o->textDraft.color=o->ink;
    }
    o->editPos=o->textDraft.pos;o->tool=T_TEXT;o->ignoreTextEscape=false;
    SetFocus(o->hwnd);edit_attach_ime(o);edit_poll_start(o->hwnd);
    SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    logf(L"文字编辑开始：索引=%d 位置=(%.1f,%.1f) 字号=%d 拉伸=(%.3f,%.3f) 角度=%.1f，鼠标使用箭头，保留文字插入竖线及边框移动/拉伸/旋转指针",
        index,o->editPos.X,o->editPos.Y,o->toolSizes[T_TEXT-T_RECT],
        o->textDraft.textScaleX,o->textDraft.textScaleY,o->textDraft.textAngle);
    InvalidateRect(o->hwnd,nullptr,FALSE);
}
static void commit_text(Overlay* o){
    if(!o->editing)return;
    Ann draft=editing_text(o);o->editing=false;
    size_t begin=draft.text.find_first_not_of(L" \t\r\n"),end=draft.text.find_last_not_of(L" \t\r\n");
    draft.text=begin==std::wstring::npos?L"":draft.text.substr(begin,end-begin+1);
    int index=o->editingIndex;
    if(index>=0){
        Ann before=o->anns[index];
        if(draft.text.empty()){
            o->anns.erase(o->anns.begin()+index);record_annotation_change(o,index,before,std::nullopt);
            o->selectedText=-1;
        }else{
            if(!same_text(before,draft)){o->anns[index]=draft;record_annotation_change(o,index,before,draft);}
            o->selectedText=index;
        }
    }else if(!draft.text.empty()){
        add_annotation(o,draft);o->selectedText=(int)o->anns.size()-1;
    }else o->selectedText=-1;
    o->editingIndex=-1;o->editText.clear();edit_detach_ime(o);edit_poll_stop(o->hwnd);
    logf(L"文字编辑提交：索引=%d 字符数=%zu 拉伸=(%.3f,%.3f) 角度=%.1f",
        o->selectedText,draft.text.size(),draft.textScaleX,draft.textScaleY,draft.textAngle);
    o->selectedText=-1;
    InvalidateRect(o->hwnd,nullptr,FALSE);
}
static void cancel_text(Overlay* o){
    if(!o->editing)return;
    logf(L"文字编辑取消：索引=%d 字符数=%zu，保留原文字",o->editingIndex,o->editText.size());
    o->selectedText=o->editingIndex;o->editingIndex=-1;o->editing=false;o->editText.clear();
    edit_detach_ime(o);edit_poll_stop(o->hwnd);InvalidateRect(o->hwnd,nullptr,FALSE);
}
static bool begin_text_drag(Overlay* o,int handle,int index,POINT point){
    Ann text;
    bool resume=false;
    if(o->editing){
        text=editing_text(o);
        if(text.text.find_first_not_of(L" \t\r\n")!=std::wstring::npos){
            int previous=o->editingIndex;commit_text(o);
            index=previous>=0?previous:(int)o->anns.size()-1;
            text=o->anns[index];o->selectedText=index;resume=true;
        }
    }
    else{
        if(index<0||index>=(int)o->anns.size())return false;
        text=o->anns[index];o->selectedText=index;
    }
    SetFocus(o->hwnd);SetCapture(o->hwnd);
    if(GetCapture()!=o->hwnd){logf(L"文字变换被拒绝：无法捕获鼠标 error=%lu",GetLastError());return false;}
    auto& drag=o->textDrag;drag={};drag.kind=handle==8?3:handle==9?1:2;
    drag.handle=handle;drag.index=index;drag.editing=o->editing;drag.before=text;drag.resumeEditing=resume;
    drag.origin={(REAL)(point.x-o->sel.left),(REAL)(point.y-o->sel.top)};
    auto size=text.text_frame_size();drag.center=text.text_point({size.Width/2,size.Height/2});
    drag.angle=atan2f(drag.origin.Y-drag.center.Y,drag.origin.X-drag.center.X);
    logf(L"文字变换开始：操作=%s 索引=%d 控制点=%d 位置=(%.1f,%.1f) 拉伸=(%.3f,%.3f) 角度=%.1f",
        drag.kind==1?L"移动":drag.kind==2?L"拉伸":L"旋转",index,handle,text.pos.X,text.pos.Y,
        text.textScaleX,text.textScaleY,text.textAngle);
    invalidate_text_frame(o,text);return true;
}
static Ann current_text_drag(Overlay* o){
    return o->textDrag.editing?editing_text(o):o->anns[o->textDrag.index];
}
static void store_text_drag(Overlay* o,const Ann& text){
    if(o->textDrag.editing){o->textDraft=text;o->editPos=text.pos;}
    else o->anns[o->textDrag.index]=text;
}
static void move_text_drag(Overlay* o,POINT point){
    auto& drag=o->textDrag;if(!drag.kind)return;
    Ann old=current_text_drag(o),text=drag.before;
    PointF position((REAL)(point.x-o->sel.left),(REAL)(point.y-o->sel.top));
    if(drag.kind==1){
        text.pos.X+=position.X-drag.origin.X;text.pos.Y+=position.Y-drag.origin.Y;
    }else if(drag.kind==2){
        auto current=text.text_local(position),start=text.text_local(drag.origin);
        float dx=current.X-start.X,dy=current.Y-start.Y;
        auto size=text.text_frame_size(),natural=text.text_natural_size();
        float left=0,top=0,right=size.Width,bottom=size.Height,minimum=SF(8);
        int i=drag.handle;
        if(i==0||i==6||i==7)left=std::min(dx,right-minimum);
        if(i==2||i==3||i==4)right=std::max(right+dx,minimum);
        if(i==0||i==1||i==2)top=std::min(dy,bottom-minimum);
        if(i==4||i==5||i==6)bottom=std::max(bottom+dy,minimum);
        text.pos=text.text_point({left,top});
        text.textScaleX=(right-left)/natural.Width;text.textScaleY=(bottom-top)/natural.Height;
    }else{
        float x=position.X-drag.center.X,y=position.Y-drag.center.Y;
        if(x*x+y*y<SF(3)*SF(3))return;
        float delta=(atan2f(y,x)-drag.angle)*180/3.14159265359f;
        text.textAngle=fmodf(text.textAngle+delta+540,360)-180;
        auto size=text.text_frame_size();text.pos={0,0};
        auto offset=text.text_point({size.Width/2,size.Height/2});
        text.pos={drag.center.X-offset.X,drag.center.Y-offset.Y};
    }
    store_text_drag(o,text);invalidate_text_frame(o,old);invalidate_text_frame(o,text);
    ULONGLONG now=GetTickCount64();
    if(!same_text(old,text)&&now-drag.lastLog>=100){
        drag.lastLog=now;auto size=text.text_frame_size();
        logf(L"文字变换更新：操作=%s 位置=(%.1f,%.1f) 尺寸=(%.1f,%.1f) 拉伸=(%.3f,%.3f) 角度=%.1f",
            drag.kind==1?L"移动":drag.kind==2?L"拉伸":L"旋转",text.pos.X,text.pos.Y,size.Width,size.Height,
            text.textScaleX,text.textScaleY,text.textAngle);
    }
}
static void end_text_drag(Overlay* o,bool cancelled,const wchar_t* reason,bool resume=true){
    if(!o->textDrag.kind)return;
    auto drag=o->textDrag;Ann after=current_text_drag(o);
    if(cancelled)store_text_drag(o,drag.before);
    else if(!drag.editing&&!same_text(drag.before,after))
        record_annotation_change(o,drag.index,drag.before,after);
    o->textDrag.kind=0;if(GetCapture()==o->hwnd)ReleaseCapture();
    invalidate_text_frame(o,after);invalidate_text_frame(o,cancelled?drag.before:after);
    const Ann& result=cancelled?drag.before:after;
    logf(L"文字变换结束：索引=%d 操作=%s 取消=%d 原因=%s 位置=(%.1f,%.1f) 拉伸=(%.3f,%.3f) 角度=%.1f",
        drag.index,drag.kind==1?L"移动":drag.kind==2?L"拉伸":L"旋转",(int)cancelled,reason,result.pos.X,result.pos.Y,
        result.textScaleX,result.textScaleY,result.textAngle);
    if(resume&&drag.resumeEditing)begin_text(o,{},drag.index);
}
static bool text_mouse_down(Overlay* o,POINT point){
    if(!text_interaction_at(o,point))return false;
    int handle=text_frame_hit(o,point);
    if(handle>=0)return begin_text_drag(o,handle,o->selectedText,point);
    if(o->editing){
        auto text=editing_text(o);
        if(text.text_contains({(REAL)(point.x-o->sel.left),(REAL)(point.y-o->sel.top)})){
            SetFocus(o->hwnd);edit_attach_ime(o);return true;
        }
        commit_text(o);
    }
    o->selectedText=-1;return false;
}
static void draw_editing(Graphics& g,Overlay* o){
    if(!o->editing)return;
    Ann draft=editing_text(o);draft.draw(g,nullptr);
    auto state=g.Save();draft.apply_text_transform(g);
    Font font(FONT_UI(),SF(draft.fontSize),FontStyleRegular,UnitPixel);
    Bitmap bitmap(1,1);Graphics measure(&bitmap);RectF bounds;
    measure.MeasureString(draft.text.c_str(),-1,&font,PointF(0,0),&bounds);
    float x=draft.text.empty()?0:bounds.Width;
    auto caret=text_caret_rect({x,0},(int)draft.fontSize);
    Pen green(Color(255,7,193,96),caret.Width);g.DrawLine(&green,x,caret.Y,x,caret.GetBottom());
    if(!o->composing.empty()){
        RectF prefix;measure.MeasureString(o->editText.c_str(),-1,&font,PointF(0,0),&prefix);
        Pen underline(Color(255,7,193,96),SF(1));
        g.DrawLine(&underline,o->editText.empty()?0:prefix.Width,caret.GetBottom()+SF(1),x,caret.GetBottom()+SF(1));
    }
    g.Restore(state);
}
