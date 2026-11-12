#pragma once
// Every requested working set must fit both cache bounds. Keeping 256px sources
// at full decoded size fits only 32 entries in 8 MiB and causes endless LRU churn.
inline constexpr size_t NativeIconEntries=128;
inline constexpr UINT NativeIconEdge=128;
static_assert(NativeIconEntries*NativeIconEdge*NativeIconEdge*4<=IconLimit);
inline com_ptr<IWICBitmapSource> NativeIconRaster(IWICImagingFactory *factory,IWICBitmapSource *source,UINT &width,UINT &height)
{
    com_ptr<IWICBitmapSource> result;
    if(std::max(width,height)<=NativeIconEdge){result.copy_from(source);return result;}
    double scale=static_cast<double>(NativeIconEdge)/std::max(width,height);
    UINT w=std::max(1u,static_cast<UINT>(std::lround(width*scale))),h=std::max(1u,static_cast<UINT>(std::lround(height*scale)));
    com_ptr<IWICBitmapScaler> scaler;check_hresult(factory->CreateBitmapScaler(scaler.put()));
    check_hresult(scaler->Initialize(source,w,h,WICBitmapInterpolationModeHighQualityCubic));
    width=w;height=h;return scaler.as<IWICBitmapSource>();
}
