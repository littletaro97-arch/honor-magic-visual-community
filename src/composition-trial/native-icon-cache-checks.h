#pragma once
// Owned, offscreen fixtures exercise the production decoder and cache insertion.
// Only an owned message-only HWND is used; no desktop lease, user layout, IPC
// peer or real application is involved.
inline std::wstring CacheFixturePng(Graphics &graphics,UINT width,UINT height)
{
    std::vector<BYTE> pixels(static_cast<size_t>(width)*height*4);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){auto p=&pixels[(static_cast<size_t>(y)*width+x)*4];p[0]=40;p[1]=static_cast<BYTE>(x*255/width);p[2]=200;p[3]=(x<width/16||y<height/16)?0:255;}
    com_ptr<IStream> stream;check_hresult(CreateStreamOnHGlobal(nullptr,TRUE,stream.put()));
    com_ptr<IWICBitmapEncoder> encoder;check_hresult(graphics.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));check_hresult(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;check_hresult(encoder->CreateNewFrame(frame.put(),nullptr));check_hresult(frame->Initialize(nullptr));check_hresult(frame->SetSize(width,height));
    auto format=GUID_WICPixelFormat32bppBGRA;check_hresult(frame->SetPixelFormat(&format));check_hresult(frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data()));check_hresult(frame->Commit());check_hresult(encoder->Commit());
    STATSTG stat{};check_hresult(stream->Stat(&stat,STATFLAG_NONAME));if(stat.cbSize.QuadPart>65536)throw std::runtime_error("Fixture PNG exceeds protocol bound");
    LARGE_INTEGER zero{};check_hresult(stream->Seek(zero,STREAM_SEEK_SET,nullptr));std::vector<BYTE> bytes(static_cast<size_t>(stat.cbSize.QuadPart));ULONG read=0;check_hresult(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read));
    DWORD count=0;if(!CryptBinaryToStringW(bytes.data(),read,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&count))throw_last_error();
    std::wstring encoded(count,L'\0');if(!CryptBinaryToStringW(bytes.data(),read,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded.data(),&count))throw_last_error();encoded.resize(count);return encoded;
}
inline int CheckNativeIconCache(std::wstring const &report)
{
    int checks=0,requestBatches=0;bool passed=true;std::string failure;size_t sixtyOneBytes=0,fullBytes=0;
    try{
        App app;app.model.synthetic=true;app.graphics.Start();Window fixture(app,L"owned-icon-cache-check");
        auto expect=[&](bool condition,char const *message){++checks;if(!condition)throw std::runtime_error(message);};
        auto high=CacheFixturePng(app.graphics,256,256);
        auto deliver=[&](std::wstring const &key,std::wstring const &png){JsonObject row,result;Put(row,L"key",key);Put(row,L"png",png);JsonArray rows;rows.Append(row);result.Insert(L"icons",rows);fixture.ReceiveIcons(result);};
        for(int i=0;i<128;++i){deliver(L"item:owned-"+std::to_wstring(i),high);expect(fixture.icons.size()==static_cast<size_t>(i+1),"Visible high-resolution working set evicted itself");expect(fixture.iconBytes<=IconLimit,"Decoded cache exceeded its byte limit");if(i==60)sixtyOneBytes=fixture.iconBytes;}
        fullBytes=fixture.iconBytes;expect(fixture.icons.size()==128,"128 icons did not remain resident");
        for(int round=0;round<2;++round)for(int i=0;i<128;++i){deliver(L"item:owned-"+std::to_wstring(i),high);expect(fixture.icons.size()==128&&fixture.iconBytes==fullBytes,"Replacing a resident icon evicted unrelated visible icons");}
        for(int i=0;i<128;++i)expect(fixture.icons.contains(L"item:owned-"+std::to_wstring(i)),"Resident cache would re-request an evicted visible icon");
        fixture.ClearIcons();expect(fixture.icons.empty()&&fixture.iconBytes==0,"Cache clear retained decoded bytes");
        for(auto dimensions:{std::pair<UINT,UINT>{16,16},{48,48},{256,128},{512,128}}){
            deliver(L"item:dimensions",CacheFixturePng(app.graphics,dimensions.first,dimensions.second));auto actual=fixture.icons.at(L"item:dimensions").bitmap->GetPixelSize();
            double ratio=std::min(1.,128./std::max(dimensions.first,dimensions.second));UINT w=static_cast<UINT>(std::lround(dimensions.first*ratio)),h=static_cast<UINT>(std::lround(dimensions.second*ratio));
            expect(actual.width==w&&actual.height==h,"Icon aspect ratio or no-upscale behavior changed");fixture.ClearIcons();
        }
        JsonObject snapshot,board;JsonArray tiles,boards;Put(snapshot,L"revision",1);Put(board,L"id",std::wstring(L"owned-icon-cache-check"));
        for(int i=0;i<128;++i){JsonObject tile;Put(tile,L"key",L"item:owned-"+std::to_wstring(i));Put(tile,L"name",std::wstring(L"Owned fixture"));tiles.Append(tile);}
        board.Insert(L"tiles",tiles);board.Insert(L"placements",JsonArray{});boards.Append(board);snapshot.Insert(L"boards",boards);app.model.Read(snapshot);
        HWND owned=CreateWindowExW(0,L"STATIC",L"Owned icon scheduler fixture",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
        expect(owned!=nullptr,"Message-only fixture creation failed");fixture.hwnd=owned;
        struct OwnedWindow{Window &fixture;HWND hwnd;~OwnedWindow(){fixture.hwnd=nullptr;if(hwnd)DestroyWindow(hwnd);}} cleanup{fixture,owned};
        fixture.RequestIcons();
        while(!fixture.pendingIcons.empty()&&requestBatches<16){
            JsonObject result;JsonArray rows;for(auto const &key:fixture.pendingIcons){JsonObject row;Put(row,L"key",key);Put(row,L"png",high);rows.Append(row);}result.Insert(L"icons",rows);++requestBatches;fixture.ReceiveIcons(result);
        }
        expect(requestBatches==8&&fixture.pendingIcons.empty(),"Icon response/request chain did not settle after the visible working set arrived");
        auto requests=app.nextId;fixture.RequestIcons();expect(app.nextId==requests,"Stable visible icons started another IPC request");
        expect(fixture.icons.size()==128&&fixture.iconBytes<=IconLimit,"Scheduled working set did not remain resident");
    }catch(std::exception const &error){passed=false;failure=error.what();}catch(hresult_error const &error){passed=false;failure=to_string(error.message());}
    JsonObject result;result.Insert(L"allPass",JsonValue::CreateBooleanValue(passed));Put(result,L"checks",checks);Put(result,L"sourceEdge",256);Put(result,L"decodedEdge",NativeIconEdge);Put(result,L"residentIcons",128);Put(result,L"requestBatches",requestBatches);Put(result,L"sixtyOneIconBytes",static_cast<double>(sixtyOneBytes));Put(result,L"fullCacheBytes",static_cast<double>(fullBytes));Put(result,L"limitBytes",static_cast<double>(IconLimit));Put(result,L"error",to_hstring(failure).c_str());Put(result,L"scope",std::wstring(L"Owned message-only HWND and production cache/request path; no desktop takeover, IPC peer or user data"));
    std::ofstream file{std::filesystem::path(report)};if(!file)throw std::runtime_error("Cannot create cache check report");file<<to_string(result.Stringify());return passed?0:1;
}
