using System;
using System.IO;
using System.Runtime.InteropServices;

namespace MagicCommunity
{
    // Extract an icon at its intended raster resolution. ICONONLY never reads document thumbnails.
    internal static class ShellIconImageFactory
    {
        internal const int PreferredPixels = 256;
        internal static string? Read(string path)
        {
            if (string.IsNullOrWhiteSpace(path) || path.StartsWith(@"\\", StringComparison.Ordinal))
                return null;
            if (!path.StartsWith("shell:", StringComparison.OrdinalIgnoreCase) &&
                !path.StartsWith("::{", StringComparison.Ordinal) &&
                (!Path.IsPathRooted(path) || (Path.GetPathRoot(path)?.Length ?? 0) < 3))
                return null;
            Guid iid = typeof(IShellItemImageFactory).GUID;
            IShellItemImageFactory? factory = null;
            try
            {
                if (!path.StartsWith("shell:", StringComparison.OrdinalIgnoreCase) &&
                    !path.StartsWith("::{", StringComparison.Ordinal))
                {
                    // Preserve the desktop reader's local, fixed-drive boundary.
                    if (new DriveInfo(Path.GetPathRoot(path)!).DriveType != DriveType.Fixed ||
                        (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                        return null;
                }
                int hr = SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out factory);
                if (hr < 0 || factory == null) return null;
                foreach (int pixels in new[] { PreferredPixels, 128 })
                {
                    IntPtr bitmap = IntPtr.Zero;
                    try
                    {
                        // SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY: let Shell select the resource representation.
                        if (factory.GetImage(new Size(pixels, pixels), 0x1u | 0x4u, out bitmap) < 0 ||
                            bitmap == IntPtr.Zero) continue;
                        string? image = NativeMenuBitmap.Encode(bitmap, false);
                        // Conservative base64 bound: decoded PNG must fit the renderer's 64 KiB limit.
                        if (image != null && image.Length <= 22 + 4 * (65536 / 3)) return image;
                    }
                    finally { if (bitmap != IntPtr.Zero) DeleteObject(bitmap); }
                }
                return null;
            }
            catch (Exception error) when (error is COMException || error is ArgumentException ||
                                         error is IOException || error is UnauthorizedAccessException)
            { return null; }
            finally { if (factory != null) Marshal.FinalReleaseComObject(factory); }
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct Size
        {
            public int Width, Height;
            public Size(int width, int height) { Width = width; Height = height; }
        }
        [ComImport, Guid("BCC18B79-BA16-442F-80C4-8A59C30C463B"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IShellItemImageFactory
        {
            [PreserveSig] int GetImage(Size size, uint flags, out IntPtr bitmap);
        }
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        private static extern int SHCreateItemFromParsingName(string path, IntPtr bind, ref Guid iid,
            out IShellItemImageFactory factory);
        [DllImport("gdi32.dll")]
        private static extern bool DeleteObject(IntPtr bitmap);
    }
}
