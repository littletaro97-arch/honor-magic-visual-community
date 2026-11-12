#pragma once
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <commctrl.h>
#include <algorithm>
#include <vector>
#include "../shared/native-ui-font-dwrite.h"

// On-demand Win32 dialog, using the same transient DWM material, font and
// neutral light/dark surfaces as NativeWindowMaterial / NativeWpfTheme.
// Native STATIC/BUTTON HWNDs retain tab order, invocation and accessibility.
struct PermissionPresentation {
    winrt::com_ptr<ID2D1Factory> factory;
    winrt::com_ptr<IDWriteFactory> write;
    NativeUiFont::DirectWriteFonts fonts;
    winrt::com_ptr<ID2D1DCRenderTarget> target;
    bool dark=false,glass=false,saver=false,modernSaver=false;HFONT font=nullptr;
    std::vector<HPOWERNOTIFY> powerTokens;
    inline static GUID energy={0x550e8400,0xe29b,0x41d4,{0xa7,0x16,0x44,0x66,0x55,0x44,0,0}};
    inline static GUID legacy={0xe00958c0,0xc213,0x4ace,{0xac,0x77,0xfe,0xcc,0xed,0x2e,0xee,0xa5}};
    ~PermissionPresentation(){for(auto token:powerTokens)UnregisterPowerSettingNotification(token);if(font)DeleteObject(font);}
    static DWORD Preference(wchar_t const *name,DWORD fallback){DWORD v=fallback,bytes=sizeof(v);RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",name,RRF_RT_REG_DWORD,nullptr,&v,&bytes);return v;}
    void Initialize(HWND h){
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.put())));fonts.Initialize(write.get());
        auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(factory->CreateDCRenderTarget(&props,target.put()));
        SYSTEM_POWER_STATUS power{};if(GetSystemPowerStatus(&power))saver=power.SystemStatusFlag==1;
        for(auto id:{energy,legacy})if(auto token=RegisterPowerSettingNotification(h,&id,DEVICE_NOTIFY_WINDOW_HANDLE))powerTokens.push_back(token);
        Apply(h);Layout(h);
    }
    void Apply(HWND h){
        dark=Preference(L"AppsUseLightTheme",1)==0;HIGHCONTRASTW contrast{sizeof(contrast)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        bool enabled=Preference(L"EnableTransparency",1)&&!saver&&!(contrast.dwFlags&HCF_HIGHCONTRASTON)&&!GetSystemMetrics(SM_REMOTESESSION);
        BOOL darkness=dark;int corners=2,backdrop=enabled?3:1;COLORREF edge=dark?RGB(75,75,75):RGB(190,187,184);
        DwmSetWindowAttribute(h,20,&darkness,sizeof(darkness));DwmSetWindowAttribute(h,33,&corners,sizeof(corners));DwmSetWindowAttribute(h,34,&edge,sizeof(edge));
        MARGINS margins{enabled?-1:0,enabled?-1:0,enabled?-1:0,enabled?-1:0};
        glass=enabled&&SUCCEEDED(DwmSetWindowAttribute(h,38,&backdrop,sizeof(backdrop)))&&SUCCEEDED(DwmExtendFrameIntoClientArea(h,&margins));
        if(!glass){backdrop=1;margins={};DwmSetWindowAttribute(h,38,&backdrop,sizeof(backdrop));DwmExtendFrameIntoClientArea(h,&margins);}
        RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    void Power(HWND h,POWERBROADCAST_SETTING const *setting){
        if(!setting||setting->DataLength!=sizeof(DWORD))return;DWORD v=0;memcpy(&v,setting->Data,sizeof(v));bool modern=setting->PowerSetting==energy;
        if(!modern&&(setting->PowerSetting!=legacy||modernSaver)||v>(modern?2u:1u))return;saver=v!=0;modernSaver|=modern;Apply(h);
    }
    void Layout(HWND h){
        UINT dpi=GetDpiForWindow(h);auto px=[dpi](int dip){return MulDiv(dip,dpi,96);};RECT client{};GetClientRect(h,&client);int width=client.right;
        auto next=CreateFontW(-px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,NativeUiFont::Family().c_str());
        for(int id:{1,2,3}){auto child=GetDlgItem(h,id);SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(next),FALSE);}
        if(font)DeleteObject(font);font=next;
        MoveWindow(GetDlgItem(h,3),px(24),px(56),width-px(48),px(88),TRUE);
        MoveWindow(GetDlgItem(h,1),px(24),px(162),px(184),px(36),TRUE);
        MoveWindow(GetDlgItem(h,2),px(218),px(162),width-px(242),px(36),TRUE);
        InvalidateRect(h,nullptr,FALSE);
    }
    void Text(std::wstring const &text,D2D1_RECT_F r,float size,D2D1_COLOR_F color,bool center=false,bool bold=false){
        winrt::com_ptr<IDWriteTextFormat> f;winrt::check_hresult(write->CreateTextFormat(fonts.family.c_str(),fonts.collection.get(),bold?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-CN",f.put()));
        f->SetTextAlignment(center?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING);f->SetParagraphAlignment(center?DWRITE_PARAGRAPH_ALIGNMENT_CENTER:DWRITE_PARAGRAPH_ALIGNMENT_NEAR);f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        winrt::com_ptr<ID2D1SolidColorBrush> ink;target->CreateSolidColorBrush(color,ink.put());target->DrawTextW(text.c_str(),static_cast<UINT32>(text.size()),f.get(),r,ink.get());
    }
    // Alpha-preserving 32-bit DIB -> DWM frame. Only repaints on Windows events.
    void Paint(HWND h,HDC destination,RECT area,int role,std::wstring const &text=L"",UINT state=0){
        int width=area.right-area.left,height=area.bottom-area.top;if(width<1||height<1)return;
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};void *pixels=nullptr;
        struct Buffer{HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;~Buffer(){if(previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}} buffer;
        buffer.bitmap=CreateDIBSection(destination,&info,DIB_RGB_COLORS,&pixels,nullptr,0);buffer.dc=CreateCompatibleDC(destination);if(!buffer.bitmap||!buffer.dc)winrt::throw_last_error();buffer.previous=SelectObject(buffer.dc,buffer.bitmap);auto dc=buffer.dc;
        RECT bounds{0,0,width,height};winrt::check_hresult(target->BindDC(dc,&bounds));float scale=GetDpiForWindow(h)/96.f;target->SetDpi(96*scale,96*scale);float w=width/scale,hh=height/scale;
        target->BeginDraw();target->Clear(D2D1::ColorF(dark?0x202020:0xffffff,glass?(dark?224.f:230.f)/255:1.f));target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        D2D1_COLOR_F ink=D2D1::ColorF(dark?0xeeeeee:0x282828);winrt::com_ptr<ID2D1SolidColorBrush> brush;target->CreateSolidColorBrush(D2D1::ColorF(0,0),brush.put());
        if(role==0)Text(L"通知权限",{24,18,w-24,48},19,ink,false,true);
        else if(role==3)Text(text,{0,0,w,hh},13,ink);
        else{
            bool disabled=(state&ODS_DISABLED)!=0,pressed=(state&ODS_SELECTED)!=0,hot=(state&ODS_HOTLIGHT)!=0;
            auto rect=D2D1::RoundedRect({.5f,.5f,w-.5f,hh-.5f},6,6);
            brush->SetColor(role==1?D2D1::ColorF(0x0078d4,disabled?.4f:pressed?.8f:hot?.9f:1.f):D2D1::ColorF(dark?0xffffff:0x101010,pressed?.12f:hot?.08f:.04f));target->FillRoundedRectangle(rect,brush.get());
            brush->SetColor(D2D1::ColorF(dark?0xffffff:0x14171c,.13f));target->DrawRoundedRectangle(rect,brush.get(),1);
            if(state&ODS_FOCUS){brush->SetColor(D2D1::ColorF(dark?0xffffff:0x282828,.55f));target->DrawRoundedRectangle(D2D1::RoundedRect({3,3,w-3,hh-3},4,4),brush.get(),1);}
            auto foreground=role==1?D2D1::ColorF(0xffffff):ink;if(disabled)foreground.a=.55f;Text(text,{8,0,w-8,hh},12.5f,foreground,true);
        }
        HRESULT status=target->EndDraw();if(SUCCEEDED(status))BitBlt(destination,area.left,area.top,width,height,dc,0,0,SRCCOPY);
        winrt::check_hresult(status);
    }
    void Fallback(HWND h,HDC dc,RECT area,int role,std::wstring const &text){
        // A drawing failure must not make the authorization controls unusable.
        glass=false;int backdrop=1;MARGINS margins{};DwmSetWindowAttribute(h,38,&backdrop,sizeof(backdrop));DwmExtendFrameIntoClientArea(h,&margins);
        auto fill=CreateSolidBrush(dark?RGB(32,32,32):RGB(255,255,255));FillRect(dc,&area,fill);DeleteObject(fill);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,dark?RGB(238,238,238):RGB(40,40,40));auto previous=SelectObject(dc,font);
        std::wstring label=role==0?L"通知权限":text;RECT content=area;InflateRect(&content,-8,-8);DrawTextW(dc,label.c_str(),static_cast<int>(label.size()),&content,role==1||role==2?DT_CENTER|DT_VCENTER|DT_SINGLELINE:DT_WORDBREAK);SelectObject(dc,previous);
    }
    static LRESULT CALLBACK Control(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){
        if(m==WM_MOUSEMOVE){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);if(!GetPropW(h,L"MagicDialogHover")){SetPropW(h,L"MagicDialogHover",reinterpret_cast<HANDLE>(1));InvalidateRect(h,nullptr,FALSE);}}
        if(m==WM_MOUSELEAVE||m==WM_NCDESTROY){RemovePropW(h,L"MagicDialogHover");InvalidateRect(h,nullptr,FALSE);}
        if(m==WM_ERASEBKGND)return 1;
        return DefSubclassProc(h,m,w,l);
    }
};
