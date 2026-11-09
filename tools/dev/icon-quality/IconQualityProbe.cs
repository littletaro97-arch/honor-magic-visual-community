using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
namespace MagicCommunity {
public static class IconQualityProbe {
    [STAThread] public static int Main(string[] args) {
        try {
            string output=Path.GetFullPath(args[0]);Directory.CreateDirectory(output);
            // A new path avoids reusing Windows Shell's icon cache from an earlier fixture design.
            string fixture=Path.Combine(output,"multi-resolution-fixture-"+Guid.NewGuid().ToString("N")+".ico");CreateFixture(fixture);
            string? upgraded=ShellIconImageFactory.Read(fixture);
            if(upgraded==null)throw new Exception("High resolution icon not returned.");
            byte[] bytes=Convert.FromBase64String(upgraded.Substring(upgraded.IndexOf(',')+1));
            File.WriteAllBytes(Path.Combine(output,"after.png"),bytes);
            BitmapFrame improved=BitmapFrame.Create(new MemoryStream(bytes),BitmapCreateOptions.PreservePixelFormat,BitmapCacheOption.OnLoad);
            var info=new ShellInfo();
            if(SHGetFileInfo(fixture,0,ref info,(uint)Marshal.SizeOf(typeof(ShellInfo)),0x100)==IntPtr.Zero||info.Icon==IntPtr.Zero)throw new Exception("Baseline extraction failed.");
            BitmapSource baseline;
            try {baseline=Imaging.CreateBitmapSourceFromHIcon(info.Icon,Int32Rect.Empty,BitmapSizeOptions.FromEmptyOptions());baseline.Freeze();}
            finally {DestroyIcon(info.Icon);}
            Save(baseline,Path.Combine(output,"before.png"));
            bool sizePass=improved.PixelWidth>=128&&improved.PixelWidth>baseline.PixelWidth;
            var pixels=new byte[improved.PixelWidth*improved.PixelHeight*4];
            new FormatConvertedBitmap(improved,PixelFormats.Bgra32,null,0).CopyPixels(pixels,improved.PixelWidth*4,0);
            bool transparent=pixels[3]==0,opaque=pixels[(improved.PixelHeight/2*improved.PixelWidth+improved.PixelWidth/2)*4+3]==255;
            bool priorAlpha=NativeMenuBitmap.TestAlphaAndOwnership();
            bool opaqueBitmap=TestOpaqueBitmap();
            bool pngBound=bytes.Length<=65536;
            var visual=new DrawingVisual();
            using(var draw=visual.RenderOpen()) {
                draw.DrawRectangle(Brushes.White,null,new Rect(0,0,460,210));
                draw.DrawText(new FormattedText("Before: "+baseline.PixelWidth+" px",System.Globalization.CultureInfo.InvariantCulture,FlowDirection.LeftToRight,new Typeface("Segoe UI"),16,Brushes.Black,1),new Point(20,12));
                draw.DrawText(new FormattedText("After: "+improved.PixelWidth+" px",System.Globalization.CultureInfo.InvariantCulture,FlowDirection.LeftToRight,new Typeface("Segoe UI"),16,Brushes.Black,1),new Point(250,12));
                draw.DrawImage(baseline,new Rect(20,50,128,128));draw.DrawImage(improved,new Rect(250,50,128,128));
            }
            var image=new RenderTargetBitmap(460,210,96,96,PixelFormats.Pbgra32);image.Render(visual);Save(image,Path.Combine(output,"comparison.png"));
            bool pass=sizePass&&transparent&&opaque&&priorAlpha&&opaqueBitmap&&pngBound;
            string json=System.Text.Json.JsonSerializer.Serialize(new{passed=pass,beforePixels=baseline.PixelWidth,afterPixels=improved.PixelWidth,transparencyPreserved=transparent,opaquePixelsPreserved=opaque,existingAlphaOwnershipTest=priorAlpha,opaqueBitmapAccepted=opaqueBitmap,pngWithinProtocolBound=pngBound});
            File.WriteAllText(Path.Combine(output,"managed-results.json"),json);Console.WriteLine(json);return pass?0:1;
        }catch(Exception error){Console.Error.WriteLine(error);return 1;}
    }
    static void Save(BitmapSource source,string file){var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(source));using(var stream=File.Create(file))png.Save(stream);}
    static byte[] Frame(int size) {
        var bytes=new byte[size*size*4];for(int y=0;y<size;y++)for(int x=0;x<size;x++){int i=(y*size+x)*4;bool white=((x*8/size+y*8/size)%2)==0;bytes[i]=white?(byte)255:(byte)40;bytes[i+1]=white?(byte)255:(byte)80;bytes[i+2]=white?(byte)255:(byte)220;bytes[i+3]=(x<size/16||y<size/16)?(byte)0:(byte)255;}
        var bitmap=BitmapSource.Create(size,size,96,96,PixelFormats.Bgra32,null,bytes,size*4);var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(bitmap));using(var stream=new MemoryStream()){png.Save(stream);return stream.ToArray();}
    }
    static void CreateFixture(string path) {
        int[] sizes={16,256};byte[][] frames={Frame(16),Frame(256)};
        using(var writer=new BinaryWriter(File.Create(path))) {
            writer.Write((ushort)0);writer.Write((ushort)1);writer.Write((ushort)2);int offset=6+16*2;
            for(int i=0;i<2;i++){writer.Write((byte)(sizes[i]==256?0:sizes[i]));writer.Write((byte)(sizes[i]==256?0:sizes[i]));writer.Write((byte)0);writer.Write((byte)0);writer.Write((ushort)1);writer.Write((ushort)32);writer.Write(frames[i].Length);writer.Write(offset);offset+=frames[i].Length;}
            foreach(var frame in frames)writer.Write(frame);
        }
    }
    static bool TestOpaqueBitmap() {
        var info=new BitmapHeader{Size=40,Width=2,Height=-1,Planes=1,Bits=32};
        IntPtr bitmap=CreateDIBSection(IntPtr.Zero,ref info,0,out var pixels,IntPtr.Zero,0);
        if(bitmap==IntPtr.Zero)throw new Exception("Opaque bitmap fixture allocation failed.");
        try {
            Marshal.Copy(new byte[]{20,60,160,255,30,90,220,255},0,pixels,8);
            string? image=NativeMenuBitmap.Encode(bitmap,false);
            if(image==null||NativeMenuBitmap.Encode(bitmap)!=null||GetObjectType(bitmap)!=7)return false;
            var png=Convert.FromBase64String(image.Substring(image.IndexOf(',')+1));
            var frame=BitmapFrame.Create(new MemoryStream(png),BitmapCreateOptions.PreservePixelFormat,BitmapCacheOption.OnLoad);
            var decoded=new FormatConvertedBitmap(frame,PixelFormats.Bgra32,null,0);var values=new byte[8];decoded.CopyPixels(values,8,0);
            return values[2]==160&&values[3]==255&&values[6]==220&&values[7]==255;
        }finally{DeleteObject(bitmap);}
    }
    [StructLayout(LayoutKind.Sequential)]struct BitmapHeader{public uint Size;public int Width,Height;public ushort Planes,Bits;public uint Compression,ImageSize;public int X,Y;public uint Used,Important;}
    [DllImport("gdi32.dll")]static extern IntPtr CreateDIBSection(IntPtr dc,ref BitmapHeader info,uint usage,out IntPtr pixels,IntPtr section,uint offset);
    [DllImport("gdi32.dll")]static extern bool DeleteObject(IntPtr bitmap);
    [DllImport("gdi32.dll")]static extern uint GetObjectType(IntPtr bitmap);
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct ShellInfo{public IntPtr Icon;public int Index;public uint Attributes;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)]public string Name;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=80)]public string Type;}
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]static extern IntPtr SHGetFileInfo(string path,uint attrs,ref ShellInfo info,uint size,uint flags);
    [DllImport("user32.dll")]static extern bool DestroyIcon(IntPtr icon);
}}
