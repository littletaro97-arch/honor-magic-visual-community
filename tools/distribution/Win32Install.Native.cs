using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Security.Principal;
using System.Text;

namespace MagicCommunity.Setup
{
    public sealed class ProcessIdentity
    {
        public int Id, Session;
        public long Created;
        public string Path, Sid;
    }
    public sealed class LinkIdentity
    {
        public string Target, Arguments, Directory, Icon, ParsingName;
    }
    public sealed class FileUser { public int Id, Session; }
    // .NET Framework-compatible: setup runs this helper under Windows PowerShell
    // before our bundled runtime is installed. It never launches a link target.
    public static class InstallNative
    {
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] private static extern uint GetDriveType(string root);
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] private static extern uint QueryDosDevice(string name,StringBuilder target,int size);
        public static bool IsFixedVolumeRoot(string path) {
            if(path==null||path.Length!=3||!char.IsLetter(path[0])||path[1]!=':'||path[2]!='\\'||GetDriveType(path)!=3)return false;
            var target=new StringBuilder(32768);
            if(QueryDosDevice(path.Substring(0,2),target,target.Capacity)==0)return false;
            const string prefix=@"\Device\HarddiskVolume";string value=target.ToString();
            if(!value.StartsWith(prefix,StringComparison.OrdinalIgnoreCase)||value.Length==prefix.Length)return false;
            for(int i=prefix.Length;i<value.Length;i++)if(value[i]<'0'||value[i]>'9')return false;
            return true; // Reject SUBST, network drives and caller-controlled aliases.
        }
        [StructLayout(LayoutKind.Sequential)] private struct RmProcess { public uint Id; public System.Runtime.InteropServices.ComTypes.FILETIME Created; }
        [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] private struct RmInfo {
            public RmProcess Process;
            [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)] public string Application;
            [MarshalAs(UnmanagedType.ByValTStr,SizeConst=64)] public string Service;
            public uint Type,Status,Session;
            [MarshalAs(UnmanagedType.Bool)] public bool Restartable;
        }
        [DllImport("rstrtmgr.dll",CharSet=CharSet.Unicode)] private static extern int RmStartSession(out uint session,uint flags,StringBuilder key);
        [DllImport("rstrtmgr.dll",CharSet=CharSet.Unicode)] private static extern int RmRegisterResources(uint session,uint files,[MarshalAs(UnmanagedType.LPArray,ArraySubType=UnmanagedType.LPWStr)] string[] names,uint processes,IntPtr process,uint services,IntPtr service);
        [DllImport("rstrtmgr.dll",CharSet=CharSet.Unicode)] private static extern int RmGetList(uint session,out uint needed,ref uint count,[In,Out] RmInfo[] info,out uint reasons);
        [DllImport("rstrtmgr.dll")] private static extern int RmEndSession(uint session);
        public static FileUser[] FileUsers(string[] files) {
            if(files==null||files.Length==0)return new FileUser[0];
            if(files.Length>256)throw new InvalidOperationException("安装根目录可执行文件数量异常。");
            uint session;int code=RmStartSession(out session,0,new StringBuilder(33));
            if(code!=0)throw new System.ComponentModel.Win32Exception(code,"无法检查安装文件占用。");
            try {
                code=RmRegisterResources(session,(uint)files.Length,files,0,IntPtr.Zero,0,IntPtr.Zero);
                if(code!=0)throw new System.ComponentModel.Win32Exception(code,"无法注册安装文件占用检查。");
                uint needed=0,count=0,reasons=0;RmInfo[] entries=null;
                for(int attempt=0;attempt<4;attempt++){
                    code=RmGetList(session,out needed,ref count,entries,out reasons);
                    if(code==0){
                        if(count==0&&reasons!=0)throw new InvalidOperationException("系统组件仍占用安装文件，尚未开始替换。");
                        var result=new FileUser[count];
                        for(int i=0;i<count;i++)result[i]=new FileUser{Id=(int)entries[i].Process.Id,Session=(int)entries[i].Session};
                        return result;
                    }
                    if(code!=234)throw new System.ComponentModel.Win32Exception(code,"安装文件占用检查失败。");
                    if(needed>1024)throw new InvalidOperationException("安装文件占用进程过多，未开始替换。");
                    count=needed;entries=new RmInfo[count];
                }
                throw new InvalidOperationException("安装文件占用状态持续变化，请在安装器重试。");
            }finally{RmEndSession(session);}
        }
        [DllImport("advapi32.dll", SetLastError=true)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern IntPtr OpenProcess(uint access,bool inherit,int id);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern uint GetProcessId(IntPtr process);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern bool GetProcessTimes(IntPtr process,out long created,out long exited,out long kernel,out long user);
        [DllImport("advapi32.dll", SetLastError=true)] private static extern bool GetTokenInformation(IntPtr token,int kind,out uint value,int length,out int returned);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder path, ref int length);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint process);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern bool GetNamedPipeServerSessionId(IntPtr pipe, out uint session);
        public static ProcessIdentity ReadProcess(int id)
        {
            IntPtr process=OpenProcess(0x1000,false,id);
            if(process==IntPtr.Zero)throw new System.ComponentModel.Win32Exception();
            try{return ReadProcess(process);}finally{CloseHandle(process);}
        }
        private static ProcessIdentity ReadProcess(IntPtr process)
        {
            var path=new StringBuilder(32768); int count=path.Capacity;
            if(!QueryFullProcessImageName(process,0,path,ref count))throw new System.ComponentModel.Win32Exception();
            IntPtr token;
            if(!OpenProcessToken(process,8,out token))throw new System.ComponentModel.Win32Exception();
            try {
                long created,exited,kernel,user;uint session;int returned;
                if(!GetProcessTimes(process,out created,out exited,out kernel,out user)||!GetTokenInformation(token,12,out session,4,out returned))throw new System.ComponentModel.Win32Exception();
                using(var identity=new WindowsIdentity(token)) return new ProcessIdentity {
                Id=(int)GetProcessId(process),Session=(int)session,Created=DateTime.FromFileTimeUtc(created).Ticks,
                Path=path.ToString(),Sid=identity.User.Value}; }
            finally {CloseHandle(token);}
        }
        private static bool Same(ProcessIdentity a,ProcessIdentity b)
        {return a.Id==b.Id&&a.Created==b.Created&&a.Session==b.Session&&a.Sid==b.Sid&&string.Equals(a.Path,b.Path,StringComparison.OrdinalIgnoreCase);}
        public static void RequestQuit(string pipeName,ProcessIdentity expected)
        {
            if(expected.Sid!=WindowsIdentity.GetCurrent().User.Value||expected.Session!=Process.GetCurrentProcess().SessionId)
                throw new InvalidOperationException("退出请求仅限发起安装的 Windows 账户和会话。");
            IntPtr process=OpenProcess(0x1000,false,expected.Id);
            if(process==IntPtr.Zero)throw new System.ComponentModel.Win32Exception();
            try
            {
                if(!Same(ReadProcess(process),expected))throw new InvalidOperationException("退出目标已变化。");
                using(var pipe=new NamedPipeClientStream(".",pipeName,PipeDirection.Out,PipeOptions.Asynchronous,TokenImpersonationLevel.Identification))
                {
                    pipe.Connect(2000);
                    uint id,session;
                    if(!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(),out id)||id!=expected.Id||
                        !GetNamedPipeServerSessionId(pipe.SafePipeHandle.DangerousGetHandle(),out session)||session!=expected.Session||
                        !Same(ReadProcess(process),expected))throw new InvalidOperationException("退出管道不属于已核验的社区版实例。");
                    var bytes=Encoding.UTF8.GetBytes("quit\n");
                    var write=pipe.BeginWrite(bytes,0,bytes.Length,null,null);
                    using(write.AsyncWaitHandle) {if(!write.AsyncWaitHandle.WaitOne(2000))throw new TimeoutException("退出请求发送超时。");pipe.EndWrite(write);}
                }
            }finally{CloseHandle(process);}
        }

        [ComImport,Guid("000214F9-0000-0000-C000-000000000046"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IShellLink
        {
            void GetPath([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder path,int size,IntPtr data,uint flags);
            void GetIDList(out IntPtr id);void SetIDList(IntPtr id);
            void GetDescription([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder value,int size);void SetDescription([MarshalAs(UnmanagedType.LPWStr)]string value);
            void GetWorkingDirectory([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder value,int size);void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)]string value);
            void GetArguments([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder value,int size);void SetArguments([MarshalAs(UnmanagedType.LPWStr)]string value);
            void GetHotkey(out short value);void SetHotkey(short value);void GetShowCmd(out int value);void SetShowCmd(int value);
            void GetIconLocation([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder value,int size,out int index);void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)]string value,int index);
            void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)]string path,uint reserved);void Resolve(IntPtr window,uint flags);void SetPath([MarshalAs(UnmanagedType.LPWStr)]string path);
        }
        [DllImport("shell32.dll")]private static extern int SHGetNameFromIDList(IntPtr id,uint kind,out IntPtr value);
        private static object NewLink(){return Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("00021401-0000-0000-C000-000000000046"),true));}
        public static LinkIdentity ReadLink(string file)
        {
            object obj=NewLink();IntPtr id=IntPtr.Zero,name=IntPtr.Zero;
            try {
                ((IPersistFile)obj).Load(file,0);var link=(IShellLink)obj;
                var target=new StringBuilder(32768);var args=new StringBuilder(32768);var directory=new StringBuilder(32768);var icon=new StringBuilder(32768);int index;
                link.GetPath(target,target.Capacity,IntPtr.Zero,4);link.GetArguments(args,args.Capacity);link.GetWorkingDirectory(directory,directory.Capacity);link.GetIconLocation(icon,icon.Capacity,out index);
                link.GetIDList(out id);string parsing="";
                if(id!=IntPtr.Zero&&SHGetNameFromIDList(id,0x80028000,out name)>=0)parsing=Marshal.PtrToStringUni(name)??"";
                return new LinkIdentity{Target=target.ToString(),Arguments=args.ToString(),Directory=directory.ToString(),Icon=icon.ToString(),ParsingName=parsing};
            } finally {if(name!=IntPtr.Zero)Marshal.FreeCoTaskMem(name);if(id!=IntPtr.Zero)Marshal.FreeCoTaskMem(id);Marshal.FinalReleaseComObject(obj);}
        }
        public static void SaveLink(string file,string target,string arguments,string directory)
        {
            object obj=NewLink();
            try {var link=(IShellLink)obj;link.SetPath(target);link.SetArguments(arguments);link.SetWorkingDirectory(directory);link.SetIconLocation(target,0);link.SetShowCmd(1);((IPersistFile)obj).Save(file,true);}
            finally {Marshal.FinalReleaseComObject(obj);}
        }
        public static void ReplaceLink(string temporary,string destination){File.Replace(temporary,destination,null);}
    }
}
