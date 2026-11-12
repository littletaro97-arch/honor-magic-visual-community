#pragma once
#include "protocol.h"

// Preserve the supplied HICON's native size (up to the IPC bound). A 24px
// icon must not go through 24 -> 32 -> 24 resampling at 150% DPI.
static BOOL MagicTrayPixels(HICON icon, BYTE* out, ULONG* outWidth, ULONG* outHeight) {
    ICONINFO source; BITMAP dimensions; BITMAPINFO info; HDC dc; HBITMAP bitmap, old;
    BYTE* pixels; ULONG n, bytes; BOOL good = FALSE; int width, height, largest;
    ZeroMemory(&source, sizeof(source)); ZeroMemory(&dimensions, sizeof(dimensions));
    if (!icon || !GetIconInfo(icon, &source)) return FALSE;
    good = GetObjectW(source.hbmColor ? source.hbmColor : source.hbmMask, sizeof(dimensions), &dimensions) != 0;
    if (source.hbmColor) DeleteObject(source.hbmColor);
    if (source.hbmMask) DeleteObject(source.hbmMask);
    if (!good) return FALSE;
    width = dimensions.bmWidth; height = source.hbmColor ? dimensions.bmHeight : dimensions.bmHeight / 2;
    if (width < 1 || height < 1 || width > 32768 || height > 32768) return FALSE;
    largest = max(width, height);
    if (largest > MAGIC_TRAY_ICON_SIZE) { width = max(1, MulDiv(width, MAGIC_TRAY_ICON_SIZE, largest)); height = max(1, MulDiv(height, MAGIC_TRAY_ICON_SIZE, largest)); }
    bytes = (ULONG)width * height * 4; good = FALSE;
    ZeroMemory(out, MAGIC_TRAY_BYTES); ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height * 2; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    dc = CreateCompatibleDC(NULL); if (!dc) return FALSE;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    if (!bitmap) { DeleteDC(dc); return FALSE; }
    old = (HBITMAP)SelectObject(dc, bitmap);
    ZeroMemory(pixels, bytes); memset(pixels + bytes, 255, bytes);
    if (DrawIconEx(dc, 0, 0, icon, width, height, 0, NULL, DI_NORMAL) &&
        DrawIconEx(dc, 0, height, icon, width, height, 0, NULL, DI_NORMAL)) {
        GdiFlush();
        for (n = 0; n < bytes; n += 4) {
            int b = pixels[bytes+n] - pixels[n], g = pixels[bytes+n+1] - pixels[n+1], r = pixels[bytes+n+2] - pixels[n+2];
            int transparent = max(0, max(b, max(g, r)));
            out[n] = pixels[n]; out[n+1] = pixels[n+1]; out[n+2] = pixels[n+2]; out[n+3] = (BYTE)(255 - min(255, transparent));
        }
        *outWidth = (ULONG)width; *outHeight = (ULONG)height; good = TRUE;
    }
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); return good;
}
