#include "../../../src/tray-capture/tray-capture.c"
#include <stdio.h>
static HICON Fixture(int size) {
    BITMAPINFO info={0};BYTE *pixels;HBITMAP color,mask;ICONINFO icon={0};int x,y;
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=size;info.bmiHeader.biHeight=-size;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    color=CreateDIBSection(NULL,&info,DIB_RGB_COLORS,(void**)&pixels,NULL,0);
    if(!color)return NULL;
    for(y=0;y<size;y++)for(x=0;x<size;x++){int i=(y*size+x)*4;pixels[i]=x<2||y<2?0:30;pixels[i+1]=x<2||y<2?0:90;pixels[i+2]=x<2||y<2?0:220;pixels[i+3]=x<2||y<2?0:255;}
    mask=CreateBitmap(size,size,1,1,NULL);icon.fIcon=TRUE;icon.hbmColor=color;icon.hbmMask=mask;
    {HICON result=CreateIconIndirect(&icon);DeleteObject(color);DeleteObject(mask);return result;}
}
int main(void) {
    int sizes[]={16,32,48,64,128},i,j;BOOL good=TRUE;BYTE pixels[MAGIC_TRAY_BYTES];ULONG width,height;
    /* Exclude Windows' one-time USER/GDI initialization from the ownership check. */
    DWORD before;{HICON warm=Fixture(32);Pixels(warm,pixels,&width,&height);DestroyIcon(warm);}
    before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(i=0;i<5;i++) {
        HICON icon=Fixture(sizes[i]);int expected=min(sizes[i],MAGIC_TRAY_ICON_SIZE);
        for(j=0;j<10;j++){
            if(!Pixels(icon,pixels,&width,&height)||width!=(ULONG)expected||height!=(ULONG)expected||pixels[3]!=0||pixels[((height/2)*width+width/2)*4+3]!=255){good=FALSE;fprintf(stderr,"source=%d width=%lu height=%lu alphaCorner=%u alphaCenter=%u\n",sizes[i],width,height,pixels[3],pixels[((height/2)*width+width/2)*4+3]);}
        }
        DestroyIcon(icon);
    }
    width=height=99;if(Pixels(NULL,pixels,&width,&height)||width||height)good=FALSE;
    {DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);if(after>before+2)good=FALSE;
     printf("{\"passed\":%s,\"sourceSizes\":[16,32,48,64,128],\"maximumPixels\":%u,\"protocolVersion\":%u,\"gdiBefore\":%lu,\"gdiAfter\":%lu}\n",good?"true":"false",MAGIC_TRAY_ICON_SIZE,MAGIC_TRAY_VERSION,before,after);}
    return good?0:1;
}
