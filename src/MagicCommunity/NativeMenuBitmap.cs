using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace MagicCommunity;

public static class NativeMenuBitmap
{
    public static bool TestAlphaAndOwnership()
    {
        var info=new Header{Size=40,Width=3,Height=-1,Planes=1,Bits=32};
        IntPtr bitmap=CreateDIBSection(IntPtr.Zero,ref info,0,out var pixels,IntPtr.Zero,0);
        if(bitmap==IntPtr.Zero)throw new InvalidDataException("Menu bitmap fixture allocation failed.");
        try
        {
            Marshal.Copy(new byte[]{0,0,0,0,0,0,255,255,0,0,64,128},0,pixels,12);
            string uri=Encode(bitmap)??throw new InvalidDataException("Menu bitmap fixture conversion failed.");
            using var stream=new MemoryStream(Convert.FromBase64String(uri[(uri.IndexOf(',')+1)..]));
            var frame=BitmapDecoder.Create(stream,BitmapCreateOptions.PreservePixelFormat,BitmapCacheOption.OnLoad).Frames[0];
            var source=new FormatConvertedBitmap(frame,PixelFormats.Bgra32,null,0);byte[] bytes=new byte[12];source.CopyPixels(bytes,12,0);
            bool alphaPass=bytes[3]==0&&bytes[7]==255&&bytes[6]==255&&bytes[11]==128&&Math.Abs(bytes[10]-128)<=1;
            Marshal.Copy(new byte[12],0,pixels,12);return alphaPass&&Encode(bitmap) is null&&GetObjectType(bitmap)==7&&Encode(new IntPtr(-1)) is null;
        }
        finally{DeleteObject(bitmap);}
    }
    public static string? Encode(IntPtr bitmap,bool requireTransparency=true)
    {
        // Shell owns this handle, including its alpha channel. HBitmap-to-WPF
        // convenience conversion can discard alpha and produce black squares.
        if(bitmap.ToInt64() is >=-1 and <=12)return null;
        if(GetObjectType(bitmap)!=7)return null;
        if(GetObject(bitmap,Marshal.SizeOf<Bitmap>(),out var size)==0||size.Width is <1 or >256||size.Height is <1 or >256)return null;
        IntPtr dc=GetDC(IntPtr.Zero);if(dc==IntPtr.Zero)return null;
        try
        {
            var info=new Header{Size=40,Width=size.Width,Height=-size.Height,Planes=1,Bits=32};
            byte[] pixels=new byte[size.Width*size.Height*4];
            if(GetDIBits(dc,bitmap,0,(uint)size.Height,pixels,ref info,0)!=size.Height)return null;
            bool alpha=false,transparent=false;for(int i=3;i<pixels.Length;i+=4){if(pixels[i]!=0)alpha=true;if(pixels[i]<255)transparent=true;}
            // A legacy bitmap without alpha has no reliable transparency mask.
            // Do not manufacture an opaque black background; use the SVG fallback.
            if(!alpha||(requireTransparency&&!transparent))return null;
            var source=BitmapSource.Create(size.Width,size.Height,96,96,PixelFormats.Pbgra32,null,pixels,size.Width*4);
            var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(source));using var stream=new MemoryStream();encoder.Save(stream);
            return "data:image/png;base64,"+Convert.ToBase64String(stream.ToArray());
        }
        catch(Exception ex)when(ex is ArgumentException or ExternalException or InvalidOperationException){return null;}
        finally{ReleaseDC(IntPtr.Zero,dc);}
    }
    [StructLayout(LayoutKind.Sequential)]private struct Bitmap{public int Type,Width,Height,Stride;public ushort Planes,Bits;public IntPtr Pixels;}
    [StructLayout(LayoutKind.Sequential)]private struct Header{public uint Size;public int Width,Height;public ushort Planes,Bits;public uint Compression,ImageSize;public int X,Y;public uint Used,Important;}
    [DllImport("gdi32.dll")]private static extern uint GetObjectType(IntPtr obj);
    [DllImport("gdi32.dll")]private static extern IntPtr CreateDIBSection(IntPtr dc,ref Header info,uint usage,out IntPtr pixels,IntPtr section,uint offset);
    [DllImport("gdi32.dll")]private static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll",EntryPoint="GetObjectW")]private static extern int GetObject(IntPtr obj,int size,out Bitmap bitmap);
    [DllImport("gdi32.dll")]private static extern int GetDIBits(IntPtr dc,IntPtr bitmap,uint first,uint lines,[Out]byte[] pixels,ref Header info,uint usage);
    [DllImport("user32.dll")]private static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")]private static extern int ReleaseDC(IntPtr hwnd,IntPtr dc);
}
