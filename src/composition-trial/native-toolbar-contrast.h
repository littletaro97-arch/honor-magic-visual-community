#pragma once
// Read only two text-free scanlines INSIDE the rendered toolbar, not the
// wallpaper/theme guess. 512 pixels / 2 KiB, never persisted or transmitted.
// A low-rate visible-only sample also covers video/page changes without a
// foreground event. No frame capture, compositor rebuild or per-frame timer.
struct NativeToolbarContrast {
    static constexpr UINT_PTR Timer=107;
    static constexpr UINT Interval=1000,Columns=256;
    std::array<double,Columns> luminance{};
    bool available=false;
    static double Luminance(BYTE r,BYTE g,BYTE b){
        auto linear=[](double x){x/=255.;return x<=.04045?x/12.92:std::pow((x+.055)/1.055,2.4);};
        return .2126*linear(r)+.7152*linear(g)+.0722*linear(b);
    }
    static bool Light(double luminance,std::optional<bool> previous={}){
        // Hysteresis near the equal-contrast boundary suppresses color chatter.
        if(previous&&luminance>.16&&luminance<.20)return *previous;
        return 1.05/(luminance+.05)>=(luminance+.05)/.05;
    }
    bool Sample(HWND window,float dpi){
        RECT rect{};if(!window||!IsWindowVisible(window)||!GetClientRect(window,&rect)||rect.right<8||rect.bottom<8)return false;
        POINT origin{};if(!ClientToScreen(window,&origin))return false;
        int inset=std::clamp(static_cast<int>(std::lround(3*dpi/96)),2,static_cast<int>((rect.bottom-1)/2));
        HDC screen=GetDC(nullptr);if(!screen)return false;HDC memory=CreateCompatibleDC(screen);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=Columns;info.bmiHeader.biHeight=-2;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void *pixels=nullptr;HBITMAP bitmap=memory?CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
        bool ok=false;
        if(bitmap){auto old=SelectObject(memory,bitmap);SetStretchBltMode(memory,COLORONCOLOR);
            ok=StretchBlt(memory,0,0,Columns,1,screen,origin.x,origin.y+inset,rect.right,1,SRCCOPY|CAPTUREBLT)&&
                StretchBlt(memory,0,1,Columns,1,screen,origin.x,origin.y+rect.bottom-inset-1,rect.right,1,SRCCOPY|CAPTUREBLT);
            GdiFlush();
            if(ok){auto data=static_cast<BYTE const *>(pixels);for(UINT x=0;x<Columns;++x){double total=0;for(UINT y=0;y<2;++y){auto p=data+(y*Columns+x)*4;total+=Luminance(p[2],p[1],p[0]);}luminance[x]=total/2;}available=true;}
            SelectObject(memory,old);DeleteObject(bitmap);
        }
        if(memory)DeleteDC(memory);ReleaseDC(nullptr,screen);return ok;
    }
    double At(D2D1_RECT_F row,float width)const{
        if(width<=0)return .5;double sum=0;
        for(int i=0;i<5;++i){float x=row.left+(row.right-row.left)*(i+.5f)/5;sum+=luminance[std::clamp(static_cast<int>(x/width*Columns),0,static_cast<int>(Columns)-1)];}
        return sum/5;
    }
};

// GDI readback can wait for DWM. Keep it off the input/render thread, with at
// most two callbacks across all monitors and one outstanding job per bar.
struct NativeToolbarContrastWork {
    static constexpr UINT Message=WM_APP+148;
    inline static std::atomic<unsigned> pending{0};
    struct State {
        HWND window{};float dpi=96;RECT bounds{};NativeToolbarContrast result;
        std::atomic<bool> cancelled{false},ready{false};
    };
    static std::shared_ptr<State> Queue(HWND window,float dpi){
        unsigned count=pending.load();do{if(count>=2)return {};}while(!pending.compare_exchange_weak(count,count+1));
        std::shared_ptr<State> state;
        try{
            state=std::make_shared<State>();state->window=window;state->dpi=dpi;GetWindowRect(window,&state->bounds);
            auto context=std::make_unique<std::shared_ptr<State>>(state);
            if(!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE,void *value){
                std::unique_ptr<std::shared_ptr<State>> context(static_cast<std::shared_ptr<State>*>(value));auto state=*context;
                RECT before{},after{};DWORD process=0;GetWindowThreadProcessId(state->window,&process);
                if(!state->cancelled&&process==GetCurrentProcessId()&&GetWindowRect(state->window,&before)&&EqualRect(&before,&state->bounds)){
                    try{state->result.Sample(state->window,state->dpi);}catch(...){state->result.available=false;}
                    if(!GetWindowRect(state->window,&after)||!EqualRect(&after,&before))state->result.available=false;
                }
                --pending;state->ready.store(true,std::memory_order_release);
                if(!state->cancelled)PostMessageW(state->window,Message,reinterpret_cast<WPARAM>(state.get()),0);
            },context.get(),nullptr)){--pending;return {};}
            context.release();return state;
        }catch(...){--pending;return {};}
    }
    static void Cancel(std::shared_ptr<State> &state){if(state)state->cancelled=true;state.reset();}
};
