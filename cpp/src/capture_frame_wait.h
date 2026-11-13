#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// 所有输出轮流做非阻塞取帧，共享一份等待预算，不让静止副屏挡住其它屏。
template<class Attempt,class Clock,class Pause>
size_t wait_for_desktop_frames(size_t count,Attempt attempt,Clock now,Pause pause){
    if(!count)return 0;
    constexpr uint64_t budget=300;
    const uint64_t deadline=now()+budget;
    std::vector<bool> complete(count,false);
    size_t finished=0;
    while(finished<count){
        for(size_t i=0;i<count;++i)
            if(!complete[i]&&attempt(i)){complete[i]=true;++finished;}
        if(finished==count)break;
        const uint64_t current=now();
        if(current>=deadline)break;
        pause((unsigned)std::min<uint64_t>(4,deadline-current));
    }
    return finished;
}
