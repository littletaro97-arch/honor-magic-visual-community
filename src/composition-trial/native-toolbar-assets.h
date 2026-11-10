#pragma once
#include <d2d1svg.h>
#include "native-toolbar-icons.h"
#include "native-honor-icons.h"
#include "native-brand-assets.h"
#include "native-shell-strings.h"

// Values come from the bundled fancy-toolbar global.css/theme and dnd-kit.
// This class contains no frame timer: Composition animates cached item surfaces.
struct NativeToolbarVisual {
    ContainerVisual group{nullptr},hoverLayer{nullptr};
    float modelOpacity=1;
    float hoverScale=1;float3 hoverOffset{};ULONGLONG hoverRetireAt=0;
    SpriteVisual visual{nullptr};
    CompositionDrawingSurface surface{nullptr};
    SpriteVisual indicator{nullptr};CompositionColorBrush indicatorBrush{nullptr};
    ShapeVisual attention{nullptr};CompositionRoundedRectangleGeometry attentionGeometry{nullptr};CompositionColorBrush attentionBrush{nullptr};bool attentionMotion=false;
    Windows::UI::Color indicatorColor{};float2 indicatorSize{};std::wstring attentionState;
    ContainerVisual mediaClip{nullptr};SpriteVisual mediaTitle{nullptr};CompositionDrawingSurface mediaSurface{nullptr};
    std::wstring mediaKey;size_t mediaBytes=0;
    ShapeVisual mediaBody{nullptr};CompositionRoundedRectangleGeometry mediaGeometry{nullptr};CompositionColorBrush mediaBrush{nullptr};
    float2 mediaSize{};ULONGLONG mediaResizeAt=0;
    std::wstring rasterKey,extensionMotionKey;
    D2D1_RECT_F rect{};
    size_t bytes=0;
    ULONGLONG retireAt=0;
};
// A vendor file's artwork almost never fills its 24x24 viewport: HONOR is 1.00
// wide by 0.19 tall, samsung 1.00 by 0.15. Fitting the viewport into a square
// cell therefore squeezed a wordmark down to a hairline. The bake step records
// each file's ink box, and the painter fits that box to the cell instead.
// Style does not move the geometry, so any variant answers for its slug.
inline float NativeBrandInkAspect(std::wstring const &slug) {
    if(slug.empty())return 1;
    std::wstring prefix=L"Brand:"+slug+L":";
    for(auto const &asset:BrandSvgAssets){
        std::wstring name=asset.name;
        if(name.size()>prefix.size()&&name.compare(0,prefix.size(),prefix)==0)
            return asset.nh>0.001f?asset.nw/asset.nh:1.f;
    }
    return 1;
}
// A wide lockup earns a wider cell, but never more than this share of the bar:
// past it the corner would outgrow the modules it sits next to.
inline float NativeBrandMarkWidth(std::wstring const &slug,float iconSize) {
    float aspect=NativeBrandInkAspect(slug);
    float width=aspect*.6f*iconSize;
    return std::min(std::max(width,iconSize),iconSize*3.4f);
}
// A colour lockup is drawn with its own paint, not the bar's ink, so a vendor whose
// logo is dark by design disappears on a dark bar: ASUS ships a path with no fill
// (black), Samsung is navy, Alienware a dark violet, and FIREBAT is a dark brown
// wordmark under a red slash.
// The bake step records the share of each colour file painted near-black and the
// share painted near-white. Share, not brightness: the first version asked whether
// anything in the artwork was bright enough, and the FIREBAT slash (wide box, vivid
// red) answered yes while the letters stayed invisible. A mark that is merely dark
// but saturated - Lenovo red, HONOR's magenta-to-cyan gradient - has no void-dark
// paint at all and keeps its colour.
inline float const kBrandVoidShare=.30f;
inline bool NativeBrandColourLegible(std::wstring const &slug,bool darkBackdrop) {
    if(slug.empty())return false;
    std::wstring name=L"Brand:"+slug+L":color";
    for(auto const &asset:BrandSvgAssets)
        if(name==asset.name)return darkBackdrop?asset.voidShare<kBrandVoidShare:asset.blankShare<kBrandVoidShare;
    return false;  // no colour file at all: the mono cut is the only option anyway
}
struct NativeToolbarSvg {
    std::map<std::wstring,com_ptr<ID2D1SvgDocument>> documents;
    std::map<std::wstring,D2D1_RECT_F> inks;
    bool Draw(ID2D1DeviceContext *context,std::wstring const &requested,D2D1_RECT_F rect,D2D1_COLOR_F ink) {
        auto name=HonorReferenceGlyph(requested);
        com_ptr<ID2D1DeviceContext> c;c.copy_from(context);
        auto svgContext=c.try_as<ID2D1DeviceContext5>();if(!svgContext)return false;
        auto found=documents.find(name);
        if(found==documents.end()) {
            char const *source=nullptr;D2D1_RECT_F box{0,0,1,1};
            for(auto const &asset:HonorSvgAssets)if(name==asset.name){source=asset.svg;break;}
            if(!source)for(auto const &asset:ToolbarSvgAssets)if(name==asset.name){source=asset.svg;break;}
            // The baked fields are origin+size, not two corners: nx/ny is where the
            // ink starts and nw/nh is how much of the viewport it covers.
            if(!source)for(auto const &asset:BrandSvgAssets)if(name==asset.name){source=asset.svg;box={asset.nx,asset.ny,asset.nx+asset.nw,asset.ny+asset.nh};break;}
            if(!source)return false;
            com_ptr<IStream> stream;stream.attach(SHCreateMemStream(reinterpret_cast<BYTE const*>(source),static_cast<UINT>(strlen(source))));
            com_ptr<ID2D1SvgDocument> doc;
            if(FAILED(svgContext->CreateSvgDocument(stream.get(),{24,24},doc.put())))return false;
            if(documents.size()>=64){documents.clear();inks.clear();}
            found=documents.emplace(name,std::move(doc)).first;inks[name]=box;
        }
        com_ptr<ID2D1SvgElement> root;found->second->GetRoot(root.put());
        if(root)root->SetAttributeValue(L"color",D2D1_SVG_ATTRIBUTE_POD_TYPE_COLOR,&ink,sizeof(ink));
        D2D1_RECT_F box{0,0,1,1};auto cached=inks.find(name);if(cached!=inks.end())box=cached->second;
        float inkWidth=(box.right-box.left)*24,inkHeight=(box.bottom-box.top)*24;
        if(inkWidth<.5f)inkWidth=24;if(inkHeight<.5f)inkHeight=24;
        float cellWidth=rect.right-rect.left,cellHeight=rect.bottom-rect.top;
        // Contain, never stretch: the cell is authored at the ink's own aspect,
        // and a square cell has to keep the artwork's proportions.
        float scale=std::min(cellWidth/inkWidth,cellHeight/inkHeight);
        float offsetX=rect.left+(cellWidth-inkWidth*scale)/2-box.left*24*scale;
        float offsetY=rect.top+(cellHeight-inkHeight*scale)/2-box.top*24*scale;
        D2D1_MATRIX_3X2_F transform;context->GetTransform(&transform);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(offsetX,offsetY)*transform);
        svgContext->DrawSvgDocument(found->second.get());context->SetTransform(transform);return true;
    }
};
#include "native-clock-format.h"
inline auto &NativeShellLocaleCache(){static std::map<std::wstring,JsonObject> cache;return cache;}
inline std::wstring NativeShellText(std::wstring language,std::wstring const &key) {
    auto &cache=NativeShellLocaleCache();
    std::transform(language.begin(),language.end(),language.begin(),::towlower);
    auto select=[&](std::wstring const &lang)->std::wstring{
        auto found=cache.find(lang);if(found!=cache.end())return Str(found->second,key.c_str());
        auto asset=std::find_if(std::begin(NativeShellLocaleAssets),std::end(NativeShellLocaleAssets),[&](auto const &v){return lang==v.language;});if(asset==std::end(NativeShellLocaleAssets))return L"";
        if(cache.size()>=2)cache.clear();auto parsed=JsonObject::Parse(to_hstring(asset->json));cache.emplace(lang,parsed);return Str(parsed,key.c_str());
    };
    auto value=select(language);if(value.empty()){auto dash=language.find(L'-');if(dash!=language.npos)value=select(language.substr(0,dash));}if(value.empty())value=select(L"en");return value.empty()?key:value;

}
