using System.Collections.Concurrent;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Interop;
using System.Windows.Media.Imaging;

namespace MagicCommunity;

// One background STA, shared by all displays; no timer or UI-thread Shell call.
public sealed class DesktopIcons : IDisposable
{
    private readonly ConcurrentDictionary<string,string> cache=new();
    private readonly ConcurrentDictionary<string,string> identities=new();
    private long identityVersion;
    internal long IdentityVersion=>Interlocked.Read(ref identityVersion);
    internal string ApplicationIdentity(ApplicationIconSource source)=>identities.GetValueOrDefault("application:"+source.Stamp,"");
    internal string DesktopIdentity(DesktopItem item)=>identities.GetValueOrDefault(DesktopKey(item,false),"");
    private static string DesktopKey(DesktopItem item,bool bitmap){
        string id=(bitmap?"panel:":"")+DesktopGridActions.ItemId(item);
        if(!item.IsVirtual&&!item.IsFolder&&!item.Target.StartsWith(@"\\",StringComparison.Ordinal)&&(bitmap||Path.GetExtension(item.Target).ToLowerInvariant() is ".exe" or ".lnk" or ".url"))try{var file=new FileInfo(item.Target);if(file.Exists)id+=":"+file.Length+":"+file.LastWriteTimeUtc.Ticks;}catch(IOException){}catch(UnauthorizedAccessException){}
        return id;
    }
    private readonly BlockingCollection<(DesktopItem Item,string Id,bool Bitmap,ApplicationIconSource? Application)> queue=new(512);
    private readonly Action notify;
    private readonly Timer notifyTimer;
    private int notificationQueued;
    private void QueueNotification(){if(Interlocked.CompareExchange(ref notificationQueued,1,0)!=0)return;try{notifyTimer.Change(75,Timeout.Infinite);}catch(ObjectDisposedException){Interlocked.Exchange(ref notificationQueued,0);}}
    private long bytes,age,recycleVersion;
    private readonly ConcurrentDictionary<string,long> ages=new();
    private readonly ConcurrentDictionary<string,byte> completed=new();
    private volatile bool disposed;
    private readonly object imageGate=new();
    private readonly Dictionary<string,FileSystemWatcher> imageWatches=new(StringComparer.OrdinalIgnoreCase);
    private readonly ConcurrentDictionary<string,byte> changedImages=new(StringComparer.OrdinalIgnoreCase);
    internal event Action<string>? ImageChanged;
    internal event Action<string>? ApplicationIconFailed;
    private void ImageFileChanged(string path)
    {
        if(disposed||!watchedImages.ContainsKey(path))return;
        string prefix="panel:"+DesktopGridActions.ItemId(new DesktopItem{Target=path})+":";
        foreach(var key in cache.Keys.Where(key=>(key.StartsWith(prefix,StringComparison.Ordinal)||key.StartsWith("application:",StringComparison.Ordinal)&&key.Contains(path,StringComparison.OrdinalIgnoreCase))))
            if(cache.TryRemove(key,out var value)){Interlocked.Add(ref bytes,-value.Length*2L);ages.TryRemove(key,out _);completed.TryRemove(key,out _);identities.TryRemove(key,out _);}
        changedImages[path]=0;
        QueueNotification();
    }
    private void WatchImage(string path)
    {
        if(Path.GetExtension(path).ToLowerInvariant() is not (".png" or ".ico" or ".jpg" or ".jpeg" or ".bmp"))return;
        if(watchedImages.Count>=512&&!watchedImages.ContainsKey(path))return;
        watchedImages.TryAdd(path,0);
        string? directory=Path.GetDirectoryName(path);if(directory is null)return;
        lock(imageGate){
            if(disposed||imageWatches.ContainsKey(directory)||imageWatches.Count>=4)return;
            try{
                var watch=new FileSystemWatcher(directory){NotifyFilter=NotifyFilters.FileName|NotifyFilters.LastWrite|NotifyFilters.Size,IncludeSubdirectories=false};
                watch.Changed+=(_,e)=>ImageFileChanged(e.FullPath);watch.Created+=(_,e)=>ImageFileChanged(e.FullPath);watch.Deleted+=(_,e)=>ImageFileChanged(e.FullPath);
                watch.Renamed+=(_,e)=>{ImageFileChanged(e.OldFullPath);ImageFileChanged(e.FullPath);};
                watch.Error+=(_,_)=>{foreach(string image in watchedImages.Keys.Where(p=>StringComparer.OrdinalIgnoreCase.Equals(Path.GetDirectoryName(p),directory)))ImageFileChanged(image);};
                imageWatches.Add(directory,watch);watch.EnableRaisingEvents=true;
            }catch(IOException){}catch(UnauthorizedAccessException){}
        }
    }
    private readonly ConcurrentDictionary<string,byte> watchedImages=new(StringComparer.OrdinalIgnoreCase);
    public DesktopIcons(Action notify)
    {
        this.notify=notify;notifyTimer=new(timerState=>{Interlocked.Exchange(ref notificationQueued,0);if(!disposed){foreach(var path in changedImages.Keys)if(changedImages.TryRemove(path,out _))ImageChanged?.Invoke(path);this.notify();}},null,Timeout.Infinite,Timeout.Infinite);var thread=new Thread(Run){IsBackground=true,Name="Magic shell icons"};thread.SetApartmentState(ApartmentState.STA);thread.Start();
    }
    internal void InvalidateApplication(string identity)
    {
        string prefix="application:"+identity+"|";
        foreach(var id in cache.Keys.Where(id=>id.StartsWith(prefix,StringComparison.Ordinal)))
            if(cache.TryRemove(id,out var prior)){Interlocked.Add(ref bytes,-prior.Length*2L);ages.TryRemove(id,out _);completed.TryRemove(id,out _);identities.TryRemove(id,out _);}
    }
    internal string? GetApplication(ApplicationIconSource source)
    {
        WatchImage(source.Image);WatchImage(source.Path);
        return Cached("application:"+source.Stamp,new(){Target=source.Path},true,source);
    }
    public string? Get(DesktopItem item,bool bitmap=false)
    {
        string id=DesktopKey(item,bitmap);
        if(bitmap&&!item.IsVirtual)WatchImage(item.Target);
        if(DesktopFileDrop.IsRecycleBin(item))id+=":v"+Interlocked.Read(ref recycleVersion);
        return Cached(id,item,bitmap,null);
    }
    private string? Cached(string id,DesktopItem item,bool bitmap,ApplicationIconSource? application)
    {
        if(cache.TryGetValue(id,out var icon)){ages[id]=Interlocked.Increment(ref age);return icon.Length==0?null:icon;}
        if(cache.Count>=512){foreach(var cold in ages.OrderBy(p=>p.Value).Take(32)){if(completed.TryRemove(cold.Key,out _)&&cache.TryRemove(cold.Key,out var prior)){Interlocked.Add(ref bytes,-prior.Length*2L);ages.TryRemove(cold.Key,out _);identities.TryRemove(cold.Key,out _);}}}
        if(cache.Count>=512||disposed)return null;
        if(cache.TryAdd(id,"")){
            ages[id]=Interlocked.Increment(ref age);
            if(!queue.TryAdd((item,id,bitmap,application))){cache.TryRemove(id,out _);ages.TryRemove(id,out _);}
        }
        return null;
    }
    internal void InvalidateRecycleBin()
    {
        Interlocked.Increment(ref recycleVersion);
        string key=DesktopGridActions.ItemId(new DesktopItem{Target=ShellClipboardCommands.RecycleBinTarget});
        foreach(string id in cache.Keys.Where(id=>id.StartsWith(key+":v",StringComparison.Ordinal)||id.StartsWith("panel:"+key+":v",StringComparison.Ordinal)))
            if(cache.TryRemove(id,out var prior)){Interlocked.Add(ref bytes,-prior.Length*2L);ages.TryRemove(id,out _);completed.TryRemove(id,out _);identities.TryRemove(id,out _);}
    }
    private void Run()
    {
        int initialized=CoInitializeEx(IntPtr.Zero,2);
        try
        {
            if(initialized<0)return;
            foreach(var entry in queue.GetConsumingEnumerable())
            {
                if(disposed)break;
                try
                {
                    string resolved="";try{if(entry.Application is {Application:true} app)resolved=ApplicationIconIdentity.Resolve(app.Path,app.Umid);else if(!entry.Item.IsVirtual&&!entry.Item.IsFolder)resolved=ApplicationIconIdentity.Resolve(entry.Item.Target);}catch(Exception e)when(e is not OutOfMemoryException){}
                    if(cache.ContainsKey(entry.Id)){identities[entry.Id]=resolved;if(resolved.Length>0){if(entry.Application is null)Interlocked.Increment(ref identityVersion);if(!disposed)QueueNotification();}}
                    var icon=entry.Application is { } application?application.Read(message=>ApplicationIconFailed?.Invoke(message)):Read(entry.Item,entry.Bitmap);
                    if(icon is not null&&cache.ContainsKey(entry.Id)&&Interlocked.Read(ref bytes)+icon.Length*2<=16*1024*1024)
                    {cache[entry.Id]=icon;Interlocked.Add(ref bytes,icon.Length*2);if(!disposed)QueueNotification();}
                }
                catch(Exception ex) when(ex is not OutOfMemoryException){/* A failed icon leaves a stable placeholder; opening still uses the known reference. */}
                finally{if(cache.ContainsKey(entry.Id))completed[entry.Id]=0;}
            }
        }
        finally{if(initialized>=0)CoUninitialize();}
    }
    internal static string? Read(DesktopItem item,bool imageFile,bool resizeImage=false)
    {
        IntPtr pidl=IntPtr.Zero;
        try
        {
            var info=new FileInfoNative();uint flags=0x100; // SHGFI_ICON, large icon
            if(DesktopFileDrop.IsRecycleBin(item))
            {
                if(RecycleBinWatch.QueryFull() is bool full){
                    var stock=new StockInfo{Size=(uint)Marshal.SizeOf<StockInfo>()};
                    if(SHGetStockIconInfo(full?32u:31u,0x100,ref stock)>=0)info.Icon=stock.Icon;
                }
                if(info.Icon==IntPtr.Zero){Marshal.ThrowExceptionForHR(SHParseDisplayName(item.Target,IntPtr.Zero,out pidl,0,out _));SHGetFileInfoPidl(pidl,0,ref info,(uint)Marshal.SizeOf<FileInfoNative>(),flags|8);}
            }
            else if(item.IsVirtual)
            {
                Marshal.ThrowExceptionForHR(SHParseDisplayName(item.Target,IntPtr.Zero,out pidl,0,out _));
                SHGetFileInfoPidl(pidl,0,ref info,(uint)Marshal.SizeOf<FileInfoNative>(),flags|8);
            }
            else
            {
                if(!Path.IsPathFullyQualified(item.Target)||item.Target.StartsWith("\\\\",StringComparison.Ordinal))return null;
                if(new DriveInfo(Path.GetPathRoot(item.Target)!).DriveType!=DriveType.Fixed)return null;
                if((File.GetAttributes(item.Target)&FileAttributes.ReparsePoint)!=0)return null;
                if(imageFile&&Path.GetExtension(item.Target).ToLowerInvariant() is ".png" or ".ico" or ".jpg" or ".jpeg" or ".bmp")
                {
                    using var stream=new FileStream(item.Target,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete);
                    if(stream.Length>2*1024*1024)return null;
                    var decoder=BitmapDecoder.Create(stream,BitmapCreateOptions.DelayCreation,BitmapCacheOption.None);
                    if(decoder.Frames.Count==0)return null;var frame=decoder.Frames[0];
                    // Generic panel icons retain their 256px input bound; shell images opt into bounded resizing.
                    int limit=resizeImage?2048:256;
                    if(frame.PixelWidth<1||frame.PixelWidth>limit||frame.PixelHeight<1||frame.PixelHeight>limit)return null;
                    if(resizeImage)return NativeMenuBitmap.EncodeIcon(frame);
                    System.Windows.Media.Imaging.BitmapSource image=frame;
                    var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(image));using var output=new MemoryStream();png.Save(output);
                    return output.Length<=65536?"data:image/png;base64,"+Convert.ToBase64String(output.ToArray()):null;
                }
                // Ask Shell for a high resolution icon, not its fixed 32px
                // SHGFI_ICON fallback. Extraction stays on the existing STA and
                // inside the same 16 MiB cache; no content thumbnails are read.
                try { if (ApplicationIconSource.ShellImage(item.Target) is { } icon) return icon; }
                catch (Exception error) when (error is not OutOfMemoryException) { }
                SHGetFileInfoPath(item.Target,0,ref info,(uint)Marshal.SizeOf<FileInfoNative>(),flags);
            }
            if(info.Icon==IntPtr.Zero)return null;
            try
            {
                var bitmap=Imaging.CreateBitmapSourceFromHIcon(info.Icon,System.Windows.Int32Rect.Empty,BitmapSizeOptions.FromEmptyOptions());bitmap.Freeze();
                var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(bitmap));using var output=new MemoryStream();encoder.Save(output);
                return "data:image/png;base64,"+Convert.ToBase64String(output.ToArray());
            }
            finally{DestroyIcon(info.Icon);}
        }
        finally{if(pidl!=IntPtr.Zero)Marshal.FreeCoTaskMem(pidl);}
    }
    public void Dispose(){disposed=true;lock(imageGate){foreach(var watch in imageWatches.Values)watch.Dispose();imageWatches.Clear();}notifyTimer.Dispose();queue.CompleteAdding();}
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]private struct FileInfoNative{public IntPtr Icon;public int Index;public uint Attributes;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)]public string Name;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=80)]public string Type;}
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]private struct StockInfo{public uint Size;public IntPtr Icon;public int SystemIndex,Index;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)]public string Path;}
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]private static extern int SHGetStockIconInfo(uint id,uint flags,ref StockInfo info);
    [DllImport("ole32.dll")]private static extern int CoInitializeEx(IntPtr reserved,uint flags);
    [DllImport("ole32.dll")]private static extern void CoUninitialize();
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]private static extern int SHParseDisplayName(string name,IntPtr bind,out IntPtr pidl,uint mask,out uint attributes);
    [DllImport("shell32.dll",EntryPoint="SHGetFileInfoW",CharSet=CharSet.Unicode)]private static extern IntPtr SHGetFileInfoPath(string path,uint attributes,ref FileInfoNative info,uint size,uint flags);
    [DllImport("shell32.dll",EntryPoint="SHGetFileInfoW")]private static extern IntPtr SHGetFileInfoPidl(IntPtr pidl,uint attributes,ref FileInfoNative info,uint size,uint flags);
    [DllImport("user32.dll")]private static extern bool DestroyIcon(IntPtr icon);
}

public sealed class DesktopChanges
{
    private readonly object gate=new();
    private long version;
    private TaskCompletionSource signal=NewSignal();
    private static TaskCompletionSource NewSignal()=>new(TaskCreationOptions.RunContinuationsAsynchronously);
    public long Version{get{lock(gate)return version;}}
    public void Notify(){TaskCompletionSource prior;lock(gate){version++;prior=signal;signal=NewSignal();}prior.TrySetResult();}
    public async Task Wait(long after,CancellationToken cancellation){Task pending;lock(gate){if(version!=after)return;pending=signal.Task;}await pending.WaitAsync(TimeSpan.FromSeconds(10),cancellation).ConfigureAwait(false);}
}
