#pragma once
#include <functional>
// Observe clicks only while a transient panel is open. Never swallow input,
// poll, or dismiss/reenter a window from the low-level hook callback.
struct NativeOutsidePointer {
    HWND receiver=nullptr;
    UINT message=0;
    HHOOK hook=nullptr;
    uint64_t generation=0;
    bool queued=false;
    std::function<bool(POINT)> contains;
    inline static NativeOutsidePointer *owner=nullptr;
    ~NativeOutsidePointer(){Stop();}
    void Stop(){
        if(hook){UnhookWindowsHookEx(hook);hook=nullptr;}
        if(owner==this)owner=nullptr;
        receiver=nullptr;contains={};queued=false;++generation;
    }
    bool Start(HWND window,UINT event,std::function<bool(POINT)> inside){
        Stop();if(owner)owner->Stop();receiver=window;message=event;contains=std::move(inside);owner=this;
        hook=SetWindowsHookExW(WH_MOUSE_LL,Mouse,GetModuleHandleW(nullptr),0);
        if(!hook){Stop();return false;}return true;
    }
    void Pointer(POINT point){
        if(receiver&&!queued&&contains&&!contains(point))
            queued=PostMessageW(receiver,message,static_cast<WPARAM>(generation),0)!=FALSE;
    }
    bool Accept(uint64_t value){return queued&&value==generation;}
    static LRESULT CALLBACK Mouse(int code,WPARAM event,LPARAM data){
        if(code==HC_ACTION&&owner&&data&&(event==WM_LBUTTONDOWN||event==WM_RBUTTONDOWN||event==WM_MBUTTONDOWN||event==WM_XBUTTONDOWN))
            owner->Pointer(reinterpret_cast<MSLLHOOKSTRUCT const*>(data)->pt);
        return CallNextHookEx(nullptr,code,event,data);
    }
};
