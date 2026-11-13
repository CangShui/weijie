static const wchar_t* annotation_name(AnnKind kind){
    const wchar_t* names[]={L"矩形",L"椭圆",L"箭头",L"画笔",L"马赛克",L"文字",L"序号",L"表情"};
    return names[(int)kind];
}
static RectF annotation_badge_rect(const Overlay* o,RectF bounds){
    float width=std::min(SF(280),(float)(o->sel.right-o->sel.left));
    return {std::clamp(bounds.X-SF(3),0.0f,std::max(0.0f,(float)(o->sel.right-o->sel.left)-width)),
            std::max(0.0f,bounds.Y-SF(29)),width,SF(23)};
}
static void invalidate_annotation(Overlay* o,const Ann& ann){
    auto r=ann.bounds();float pad=SF(ann.kind==AK_TEXT?36.0f:8.0f);
    auto badge=annotation_badge_rect(o,r);
    RECT dirty{(LONG)floor(std::min(r.X-pad,badge.X))+o->sel.left,
        (LONG)floor(std::min(r.Y-pad,badge.Y))+o->sel.top,
        (LONG)ceil(std::max(r.GetRight()+pad,badge.GetRight()))+o->sel.left,
        (LONG)ceil(std::max(r.GetBottom()+pad,badge.GetBottom()))+o->sel.top};
    InvalidateRect(o->hwnd,&dirty,FALSE);
}
static void clear_annotation_selection(Overlay* o,const wchar_t* reason){
    if(o->selectedAnnotation>=0&&o->selectedAnnotation<(int)o->anns.size()){
        invalidate_annotation(o,o->anns[o->selectedAnnotation]);
        logf(L"图案选择清除：索引=%d 原因=%s",o->selectedAnnotation,reason);
    }
    o->selectedAnnotation=-1;o->annotationCycleHits.clear();o->annotationCycleValid=false;
}
static void select_annotation(Overlay* o,int index){
    if(o->selectedAnnotation>=0&&o->selectedAnnotation<(int)o->anns.size())
        invalidate_annotation(o,o->anns[o->selectedAnnotation]);
    o->selectedAnnotation=index;
    o->selectedText=-1;
    invalidate_annotation(o,o->anns[index]);
}
static std::vector<int> annotation_hits(Overlay* o,POINT point){
    std::vector<int> hits;
    PointF local((REAL)(point.x-o->sel.left),(REAL)(point.y-o->sel.top));
    for(int i=(int)o->anns.size()-1;i>=0;--i)
        if(o->anns[i].hit_test(local,SF(3)))hits.push_back(i);
    return hits;
}
static bool begin_annotation_drag(Overlay* o,POINT point){
    if(!o->alive||!o->hasSel||o->mode!=0||o->textDrag.kind||o->sizeDrag.active||
       !ptInRect(o->sel,point.x,point.y))return false;
    commit_text(o);
    auto hits=annotation_hits(o,point);
    if(hits.empty()){
        clear_annotation_selection(o,L"右键按在画布空白处");o->selectedText=-1;
        return false;
    }
    int index=hits.front();
    auto selected=std::find(hits.begin(),hits.end(),o->selectedAnnotation);
    if(selected!=hits.end())index=*selected;
    bool cycle=o->annotationCycleValid&&hits==o->annotationCycleHits&&selected!=hits.end()&&
        abs(point.x-o->annotationCyclePoint.x)<=S(4)&&abs(point.y-o->annotationCyclePoint.y)<=S(4);
    logf(L"图案右键请求到达：位置=(%ld,%ld) 命中数=%zu 目标=%d 类型=%s 可循环选择=%d；左键继续绘制",
        point.x,point.y,hits.size(),index,annotation_name(o->anns[index].kind),(int)cycle);
    o->sizeRightConsumed=true;
    close_emoji_popup(o);close_style_popup(o);SetFocus(o->hwnd);SetCapture(o->hwnd);
    if(GetCapture()!=o->hwnd){
        logf(L"图案右键请求被拒绝：无法捕获鼠标 error=%lu，未移动图案或调整工具尺寸",GetLastError());
        return true;
    }
    if(o->anns[index].kind==AK_MOSAIC){
        if(!ensure_mosaic(o)){
            if(GetCapture()==o->hwnd)ReleaseCapture();
            logf(L"图案移动被拒绝：无法生成当前选区的马赛克取样底图，保留原标注位置");
            return true;
        }
        logf(L"马赛克移动取样就绪：底图=%ldx%ld，使用目标位置像素，不搬动源图",
            o->sel.right-o->sel.left,o->sel.bottom-o->sel.top);
    }
    auto& drag=o->annotationDrag;drag={};drag.active=true;drag.index=index;drag.origin=point;
    drag.before=o->anns[index];drag.hits=std::move(hits);drag.cycle=cycle;
    select_annotation(o,index);
    hide_stamp_preview(o,L"右键选择或移动已有图案");
    logf(L"图案右键等待：索引=%d，移动超过%d像素开始拖动；只显示移动外框，不显示或响应拉伸旋转点；未拖动则选择或切换重叠对象",index,S(4));
    return true;
}
static void move_annotation_drag(Overlay* o,POINT point){
    auto& drag=o->annotationDrag;if(!drag.active)return;
    float dx=(float)(point.x-drag.origin.x),dy=(float)(point.y-drag.origin.y);
    if(!drag.moved&&dx*dx+dy*dy<SF(4)*SF(4))return;
    if(!drag.moved){
        drag.moved=true;
        auto bounds=drag.before.bounds();
        logf(L"图案移动开始：索引=%d 类型=%s 原区域=(%.1f,%.1f,%.1f,%.1f)；不改变图层、样式或工具大小",
            drag.index,annotation_name(drag.before.kind),bounds.X,bounds.Y,bounds.Width,bounds.Height);
    }
    invalidate_annotation(o,o->anns[drag.index]);
    Ann moved=drag.before;moved.translate(dx,dy);o->anns[drag.index]=std::move(moved);
    drag.dx=dx;drag.dy=dy;invalidate_annotation(o,o->anns[drag.index]);
    auto now=GetTickCount64();
    if(now-drag.lastLog>=100){
        drag.lastLog=now;
        logf(L"图案移动更新：索引=%d 相对按下位置变化=(%.1f,%.1f)截图像素",drag.index,dx,dy);
    }
}
static void end_annotation_drag(Overlay* o,bool cancelled,const wchar_t* reason,bool allowClick=true){
    if(!o->annotationDrag.active)return;
    auto drag=std::move(o->annotationDrag);o->annotationDrag={};
    Ann after=o->anns[drag.index];
    if(cancelled){
        o->anns[drag.index]=drag.before;o->annotationCycleValid=false;
    }else if(drag.moved){
        if(drag.dx!=0||drag.dy!=0)record_annotation_change(o,drag.index,drag.before,after);
        select_annotation(o,drag.index);o->annotationCycleValid=false;
    }else if(allowClick){
        int index=drag.index;
        if(drag.cycle){
            auto found=std::find(drag.hits.begin(),drag.hits.end(),index);
            index=drag.hits[(found-drag.hits.begin()+1)%drag.hits.size()];
        }
        select_annotation(o,index);
        o->annotationCycleHits=drag.hits;o->annotationCyclePoint=drag.origin;o->annotationCycleValid=true;
        logf(L"图案右键选择完成：索引=%d 类型=%s 重叠位置=%zu/%zu，未产生移动或撤销记录",
            index,annotation_name(o->anns[index].kind),
            std::find(drag.hits.begin(),drag.hits.end(),index)-drag.hits.begin()+1,drag.hits.size());
    }
    if(GetCapture()==o->hwnd)ReleaseCapture();
    invalidate_annotation(o,after);invalidate_annotation(o,o->anns[drag.index]);
    logf(L"图案右键操作返回：索引=%d 已拖动=%d 位移=(%.1f,%.1f) 取消=%d 原因=%s 捕获已释放=%d",
        drag.index,(int)drag.moved,drag.dx,drag.dy,(int)cancelled,reason,(int)(GetCapture()!=o->hwnd));
}
static void draw_annotation_selection(Graphics& g,Overlay* o){
    int index=o->selectedAnnotation;
    if(index<0||index>=(int)o->anns.size())return;
    const auto& ann=o->anns[index];auto bounds=ann.bounds();
    auto badge=annotation_badge_rect(o,bounds);bounds.Inflate(SF(3),SF(3));
    auto state=g.Save();
    g.SetClip(RectF(0,0,(REAL)(o->sel.right-o->sel.left),(REAL)(o->sel.bottom-o->sel.top)),CombineModeIntersect);
    Pen edge(Color(255,7,193,96),SF(1));edge.SetDashStyle(DashStyleDash);g.DrawRectangle(&edge,bounds);
    std::wstring label=annotation_name(ann.kind);
    auto found=std::find(o->annotationCycleHits.begin(),o->annotationCycleHits.end(),index);
    if(o->annotationCycleValid&&found!=o->annotationCycleHits.end())
        label+=L" "+std::to_wstring(found-o->annotationCycleHits.begin()+1)+L"/"+std::to_wstring(o->annotationCycleHits.size());
    label+=L" · 右拖移动 / 右击切换";
    SolidBrush background(Color(240,247,255,250));g.FillRectangle(&background,badge);
    ui::text(g,label,badge,11,Color(255,7,125,75),true);g.Restore(state);
}
