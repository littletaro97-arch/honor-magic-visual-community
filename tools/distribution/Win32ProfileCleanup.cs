using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MagicCommunity.Setup
{
    // Used only after an explicit uninstall choice. Locks every ancestor against
    // rename, rejects reparse points, and deletes the exact opened entries rather
    // than re-resolving user-controlled paths after validation.
    public sealed class ProfileCleanup : IDisposable
    {
        readonly Dictionary<string, SafeFileHandle> handles = new Dictionary<string, SafeFileHandle>(StringComparer.OrdinalIgnoreCase);
        readonly HashSet<string> deletable = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        readonly List<string> order = new List<string>();
        const uint DirectoryFlag=0x10, ReparseFlag=0x400;
        [StructLayout(LayoutKind.Sequential)] struct FileInfo {
            public uint Attributes; public System.Runtime.InteropServices.ComTypes.FILETIME Created, Accessed, Written;
            public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
        }
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern SafeFileHandle CreateFileW(string path,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle handle,out FileInfo info);
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetFileInformationByHandle(SafeFileHandle handle,int kind,ref byte value,uint size);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool GetVolumeInformationW(string root,IntPtr name,uint nameSize,out uint serial,out uint component,out uint flags,IntPtr fileSystem,uint fileSystemSize);
        public static bool UsesFilePermissions(string path) {
            uint serial,component,flags;if(!GetVolumeInformationW(Path.GetPathRoot(path),IntPtr.Zero,0,out serial,out component,out flags,IntPtr.Zero,0))throw new Win32Exception(Marshal.GetLastWin32Error());
            return (flags&8)!=0; // FILE_PERSISTENT_ACLS; FAT/exFAT has no privileged files.
        }
        static string Canonical(string path) {
            if(string.IsNullOrEmpty(path)||path.Length<3||path[1]!=':'||path[2]!='\\'||path.IndexOf('/',3)>=0||path.IndexOf(':',2)>=0)throw new InvalidDataException("清理路径不是本地绝对路径。");
            return Path.GetFullPath(path).TrimEnd('\\');
        }
        public void PinParents(string path) {
            var parents=new Stack<string>();
            for(string part=Path.GetDirectoryName(Canonical(path));part!=null;part=Path.GetDirectoryName(part))parents.Push(part);
            while(parents.Count>0)Pin(parents.Pop(),false,false);
        }
        public void Pin(string path,bool remove,bool readContents) {
            string key=Canonical(path);
            if(handles.ContainsKey(key)) { if(remove&&!deletable.Contains(key))throw new InvalidOperationException("清理句柄权限不匹配。"); return; }
            if(handles.Count>=32768)throw new InvalidDataException("数据项目过多，已停止自动清理。");
            uint access=0x80u | (remove?0x10000u:0u) | (readContents?0x80000000u:0u);
            // Readable marker/record files cannot change while checked. Directory
            // contents may change, but deletion will then fail rather than follow it.
            var handle=CreateFileW(key.Length==2?key+"\\":key,access,readContents?1u:3u,IntPtr.Zero,3,0x02200000,IntPtr.Zero);
            try {
                if(handle.IsInvalid)throw new Win32Exception(Marshal.GetLastWin32Error(),"无法固定清理目标："+key);
                FileInfo info;if(!GetFileInformationByHandle(handle,out info))throw new Win32Exception(Marshal.GetLastWin32Error());
                if((info.Attributes&ReparseFlag)!=0)throw new InvalidDataException("数据目录含链接，未执行清理："+key);
                if(readContents&&(info.Attributes&DirectoryFlag)!=0)throw new InvalidDataException("安装记录不是文件。");
                handles.Add(key,handle);order.Add(key);if(remove)deletable.Add(key);
            } catch { handle.Dispose(); throw; }
        }
        public string ReadText(string path) {
            var handle=handles[Canonical(path)];
            using(var borrowed=new SafeFileHandle(handle.DangerousGetHandle(),false))
            using(var stream=new FileStream(borrowed,FileAccess.Read)) {
                if(stream.Length>16384)throw new InvalidDataException("安装记录过大。");
                stream.Position=0;using(var reader=new StreamReader(stream))return reader.ReadToEnd();
            }
        }
        public void PlanTree(string path) {
            string root=Canonical(path);var pending=new Stack<string>();pending.Push(root);
            while(pending.Count>0) {
                string next=pending.Pop();Pin(next,true,false);
                FileInfo info;if(!GetFileInformationByHandle(handles[next],out info))throw new Win32Exception(Marshal.GetLastWin32Error());
                if((info.Attributes&DirectoryFlag)!=0)foreach(string child in Directory.EnumerateFileSystemEntries(next))pending.Push(Canonical(child));
            }
        }
        public void DeleteTree(string path) {
            string root=Canonical(path);
            for(int i=order.Count-1;i>=0;i--)if(order[i].Equals(root,StringComparison.OrdinalIgnoreCase)||order[i].StartsWith(root+"\\",StringComparison.OrdinalIgnoreCase))Delete(order[i]);
        }
        public void Delete(string path) {
            string key=Canonical(path);SafeFileHandle handle;
            if(!handles.TryGetValue(key,out handle))return;
            if(!deletable.Contains(key))throw new InvalidOperationException("清理目标未授权。");
            byte remove=1;
            if(!SetFileInformationByHandle(handle,4,ref remove,1))throw new Win32Exception(Marshal.GetLastWin32Error(),"删除未完成，已保留安装记录："+key);
            handle.Dispose();handles.Remove(key);
        }
        public void Dispose() { for(int i=order.Count-1;i>=0;i--){SafeFileHandle handle;if(handles.TryGetValue(order[i],out handle))handle.Dispose();}handles.Clear(); }
    }
}
