using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace MagicMenuDiagnostic
{
    // Standalone observer: no input injection, UIA, foreign-thread suspension or window subclassing.
    public sealed class Observer : IDisposable
    {
        public sealed class Event
        {
            public string at, kind, windowClass;
            public long ticks, hwnd;
            public uint pid, threadId, nativeTime;
            public int x, y;
            public bool injected;
        }
        public sealed class Window
        {
            public long hwnd;
            public uint pid, threadId, dpi;
            public string windowClass, response, coordinateSpace;
            public int left, top, right, bottom, error;
            public bool visible;
        }
        readonly ConcurrentQueue<Event> events = new ConcurrentQueue<Event>();
        readonly ManualResetEvent ready = new ManualResetEvent(false);
        HashSet<uint> owners;
        readonly Thread thread;
        readonly Timer deadline;
        HookProc mouseCallback;
        WinEventProc eventCallback;
        IntPtr mouseHook;
        readonly List<IntPtr> eventHooks = new List<IntPtr>();
        readonly Dictionary<IntPtr, Event> knownWindows = new Dictionary<IntPtr, Event>();
        uint hookThread;
        int queued, dropped, rightClicks, stopped;
        public int MouseHookError { get; private set; }
        public int WindowHookErrors { get; private set; }
        public bool MouseHookInstalled { get; private set; }
        public int RightClicks { get { return Volatile.Read(ref rightClicks); } }
        public int Dropped { get { return Volatile.Read(ref dropped); } }
        public bool Stopped { get { return Volatile.Read(ref stopped) != 0; } }
        public Observer(int[] pids, int seconds)
        {
            if (seconds < 1 || seconds > 600) throw new ArgumentOutOfRangeException("seconds");
            owners = new HashSet<uint>(); foreach (int pid in pids) owners.Add((uint)pid);
            thread = new Thread(Run) { IsBackground = true, Name = "Magic menu diagnostic events" };
            thread.SetApartmentState(ApartmentState.STA); thread.Start();
            if (!ready.WaitOne(3000)) throw new TimeoutException("Observer message pump did not start.");
            deadline = new Timer(delegate { Stop(); }, null, seconds * 1000, Timeout.Infinite);
        }
        void Enqueue(Event value)
        {
            if (Interlocked.Increment(ref queued) > 2048) { Interlocked.Decrement(ref queued); Interlocked.Increment(ref dropped); return; }
            events.Enqueue(value);
        }
        public void UpdateOwners(int[] pids)
        {
            var next = new HashSet<uint>(); foreach(int pid in pids) next.Add((uint)pid);
            Interlocked.Exchange(ref owners, next);
        }
        public Event[] Drain()
        {
            var result = new List<Event>(); Event value;
            while (events.TryDequeue(out value)) { Interlocked.Decrement(ref queued); result.Add(value); }
            return result.ToArray();
        }
        static Event Describe(IntPtr window, string kind, uint nativeTime)
        {
            uint pid; uint tid = GetWindowThreadProcessId(window, out pid);
            return new Event { at = DateTime.UtcNow.ToString("o"), ticks = Stopwatch.GetTimestamp(), kind = kind,
                hwnd = window.ToInt64(), pid = pid, threadId = tid, windowClass = Class(window), nativeTime = nativeTime };
        }
        IntPtr Mouse(int code, IntPtr message, IntPtr data)
        {
            // Only right-button edges. No keys, wheel, trajectories, I/O, COM or waits in this callback.
            try
            {
                long msg = message.ToInt64();
                if (code >= 0 && (msg == 0x204 || msg == 0x205))
                {
                    MouseData point = (MouseData)Marshal.PtrToStructure(data, typeof(MouseData));
                    var e = Describe(WindowFromPhysicalPoint(point.pt), msg == 0x204 ? "right-down" : "right-up", point.time);
                    e.x = point.pt.x; e.y = point.pt.y; e.injected = (point.flags & 1) != 0;
                    Enqueue(e); if (msg == 0x205) Interlocked.Increment(ref rightClicks);
                }
            }
            catch { Interlocked.Increment(ref dropped); }
            return CallNextHookEx(IntPtr.Zero, code, message, data);
        }
        void WindowEvent(IntPtr hook, uint code, IntPtr window, int objectId, int child, uint eventThread, uint time)
        {
            try
            {
                if (window == IntPtr.Zero || (code >= 0x8000 && (objectId != 0 || child != 0))) return;
                var e = Describe(window, "window-" + code.ToString("X"), time);
                Event previous;
                if(e.pid == 0 && knownWindows.TryGetValue(window,out previous))
                { e.pid=previous.pid; e.threadId=previous.threadId; e.windowClass=previous.windowClass; }
                if(code == 0x8001) knownWindows.Remove(window);
                if (code != 3 && !owners.Contains(e.pid) && e.windowClass != "#32768") return;
                if(code != 0x8001 && knownWindows.Count < 1024) knownWindows[window]=e;
                Enqueue(e);
            }
            catch { Interlocked.Increment(ref dropped); }
        }
        void Run()
        {
            try
            {
                hookThread = GetCurrentThreadId(); MSG pending; PeekMessage(out pending, IntPtr.Zero, 0, 0, 0);
                mouseCallback = Mouse; eventCallback = WindowEvent;
                mouseHook = SetWindowsHookEx(14, mouseCallback, GetModuleHandle(null), 0);
                MouseHookInstalled = mouseHook != IntPtr.Zero;
                if (!MouseHookInstalled) MouseHookError = Marshal.GetLastWin32Error();
                foreach (uint[] range in new[] { new uint[] { 3, 7 }, new uint[] { 0x8000, 0x8003 } })
                {
                    var hook = SetWinEventHook(range[0], range[1], IntPtr.Zero, eventCallback, 0, 0, 2);
                    if (hook == IntPtr.Zero) WindowHookErrors++; else eventHooks.Add(hook);
                }
                ready.Set(); MSG msg;
                while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) { TranslateMessage(ref msg); DispatchMessage(ref msg); }
            }
            finally
            {
                if (mouseHook != IntPtr.Zero) UnhookWindowsHookEx(mouseHook);
                foreach (var hook in eventHooks) UnhookWinEvent(hook);
                Interlocked.Exchange(ref stopped, 1); ready.Set();
            }
        }
        public void Stop() { if (hookThread != 0) PostThreadMessage(hookThread, 0x12, IntPtr.Zero, IntPtr.Zero); }
        public void Dispose()
        {
            if (deadline != null) deadline.Dispose(); Stop();
            if (Thread.CurrentThread != thread && !thread.Join(2000)) throw new TimeoutException("Observer shutdown timed out.");
        }
        public static Window[] Windows(int[] pids, bool probe)
        {
            var allowed = new HashSet<uint>(); foreach (int pid in pids) allowed.Add((uint)pid);
            var result = new List<Window>(); var seen = new HashSet<IntPtr>(); int probes = 0;
            EnumProc read = delegate(IntPtr hwnd, IntPtr unused)
            {
                uint pid; uint tid = GetWindowThreadProcessId(hwnd, out pid);
                if (!allowed.Contains(pid) || !seen.Add(hwnd) || result.Count >= 64) return true;
                RECT r; string coordinates; ReadPhysicalBounds(hwnd, out r, out coordinates);
                var row = new Window { hwnd = hwnd.ToInt64(), pid = pid, threadId = tid, windowClass = Class(hwnd),
                    visible = IsWindowVisible(hwnd), left = r.l, top = r.t, right = r.r, bottom = r.b, response = "not-probed", coordinateSpace = coordinates };
                try { row.dpi = GetDpiForWindow(hwnd); } catch (EntryPointNotFoundException) { }
                if (probe && row.visible && probes++ < 4)
                {
                    UIntPtr reply; SetLastError(0);
                    var ok = SendMessageTimeout(hwnd, 0, UIntPtr.Zero, IntPtr.Zero, 0x23, 40, out reply);
                    row.error = ok == IntPtr.Zero ? Marshal.GetLastWin32Error() : 0;
                    row.response = ok != IntPtr.Zero ? "responded" : row.error == 5 ? "access-denied" : "timeout-or-window-ended";
                }
                result.Add(row); return true;
            };
            EnumWindows(delegate(IntPtr hwnd, IntPtr unused) {
                read(hwnd, unused); string name = Class(hwnd);
                if (name == "WorkerW" || name == "Progman") EnumChildWindows(hwnd, read, IntPtr.Zero);
                return true;
            }, IntPtr.Zero);
            return result.ToArray();
        }
        static void ReadPhysicalBounds(IntPtr hwnd, out RECT rect, out string coordinates)
        {
            // MSLLHOOKSTRUCT is physical. Avoid mixing it with a DPI-virtualized window rectangle.
            var previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
            try { coordinates = GetWindowRect(hwnd, out rect) ? (previous != IntPtr.Zero ? "physical" : "unverified-dpi") : "unavailable"; }
            finally { if (previous != IntPtr.Zero) SetThreadDpiAwarenessContext(previous); }
        }
        public static string Token(int pid)
        {
            IntPtr process=IntPtr.Zero, token=IntPtr.Zero, data=IntPtr.Zero;
            try
            {
                process=OpenProcess(0x1000,false,pid);
                if(process==IntPtr.Zero || !OpenProcessToken(process,8,out token)) return "unavailable:"+Marshal.GetLastWin32Error();
                int size; GetTokenInformation(token,25,IntPtr.Zero,0,out size);
                if(size<16 || size>4096) return "invalid-token-size";
                data=Marshal.AllocHGlobal(size);
                if(!GetTokenInformation(token,25,data,size,out size)) return "token-error:"+Marshal.GetLastWin32Error();
                IntPtr sid=Marshal.ReadIntPtr(data);
                byte count=Marshal.ReadByte(GetSidSubAuthorityCount(sid));
                int rid=Marshal.ReadInt32(GetSidSubAuthority(sid,(uint)(count-1)));
                string level=rid>=0x4000?"system":rid>=0x3000?"high":rid>=0x2000?"medium":rid>=0x1000?"low":"untrusted";
                int ui=-1, elevation=-1;
                if(GetTokenInformation(token,26,data,4,out size)) ui=Marshal.ReadInt32(data);
                if(GetTokenInformation(token,20,data,4,out size)) elevation=Marshal.ReadInt32(data);
                return level+";uiAccess="+ui+";elevated="+elevation;
            }
            finally
            {
                if(data!=IntPtr.Zero)Marshal.FreeHGlobal(data);
                if(token!=IntPtr.Zero)CloseHandle(token);
                if(process!=IntPtr.Zero)CloseHandle(process);
            }
        }
        [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("advapi32.dll",SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
        [DllImport("advapi32.dll",SetLastError=true)] static extern bool GetTokenInformation(IntPtr token,int kind,IntPtr data,int length,out int needed);
        [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
        [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid,uint index);
        static string Class(IntPtr window) { var name = new StringBuilder(256); GetClassName(window, name, name.Capacity); return name.ToString(); }
        [StructLayout(LayoutKind.Sequential)] struct POINT { public int x, y; }
        [StructLayout(LayoutKind.Sequential)] struct MouseData { public POINT pt; public uint mouseData, flags, time; public UIntPtr extra; }
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int l, t, r, b; }
        [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public UIntPtr w; public IntPtr l; public uint time; public POINT pt; public uint reserved; }
        delegate IntPtr HookProc(int code, IntPtr message, IntPtr data);
        delegate void WinEventProc(IntPtr hook, uint code, IntPtr hwnd, int obj, int child, uint tid, uint time);
        delegate bool EnumProc(IntPtr hwnd, IntPtr context);
        [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SetWindowsHookEx(int id, HookProc fn, IntPtr module, uint tid);
        [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr hook);
        [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr w, IntPtr l);
        [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SetWinEventHook(uint first, uint last, IntPtr module, WinEventProc fn, uint pid, uint tid, uint flags);
        [DllImport("user32.dll")] static extern bool UnhookWinEvent(IntPtr hook);
        [DllImport("user32.dll")] static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
        [DllImport("user32.dll")] static extern bool PeekMessage(out MSG msg, IntPtr hwnd, uint min, uint max, uint flags);
        [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG msg);
        [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG msg);
        [DllImport("user32.dll")] static extern bool PostThreadMessage(uint tid, uint msg, IntPtr w, IntPtr l);
        [DllImport("user32.dll")] static extern IntPtr WindowFromPhysicalPoint(POINT point);
        [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder text, int size);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc fn, IntPtr context);
        [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc fn, IntPtr context);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
        [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr hwnd);
        [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, UIntPtr w, IntPtr l, uint flags, uint timeout, out UIntPtr result);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string name);
        [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
        [DllImport("kernel32.dll")] static extern void SetLastError(uint error);
    }

    // Only read-only Open/Children are public. The child is a hash-verified
    // installed menu worker, never another desktop. No production IPC is used.
    public sealed class QueryWorker : IDisposable
    {
        readonly AnonymousPipeServerStream pipe;
        readonly Process child;
        readonly StreamReader reader;
        readonly StreamWriter input;
        readonly string requestId = Guid.NewGuid().ToString("N");
        readonly object sendGate = new object();
        bool unusable, disposed;
        readonly Stopwatch life = Stopwatch.StartNew();
        readonly List<Sample> samples = new List<Sample>();
        public sealed class Sample
        {
            public string at, error;
            public long elapsedMs, privateBytes;
            public double cpuMs;
            public int threads, handles;
        }
        public sealed class Lifecycle
        {
            public string exitHex, cleanupReason, queryFailure;
            public int? exitCode;
            public bool exitedBeforeCleanup, killRequested, exitObserved;
            public Sample[] samples;
            public OwnedModuleInventory.Result loadedModules;
        }
        readonly Lifecycle outcome = new Lifecycle();
        readonly OwnedModuleInventory moduleInventory;
        public Lifecycle Evidence { get { outcome.samples=samples.ToArray();outcome.loadedModules=moduleInventory==null?null:moduleInventory.Evidence;return outcome; } }
        void SampleProcess(bool final=false)
        {
            if(samples.Count>=64 || child==null) return;
            try {
                child.Refresh();
                if(child.HasExited) { ObserveExit(0);return; }
                samples.Add(new Sample { at=DateTime.UtcNow.ToString("o"),elapsedMs=life.ElapsedMilliseconds,
                    cpuMs=child.TotalProcessorTime.TotalMilliseconds,privateBytes=child.PrivateMemorySize64,
                    threads=child.Threads.Count,handles=child.HandleCount });
                if(moduleInventory!=null)moduleInventory.Capture(life.ElapsedMilliseconds,final);
            } catch(Exception e) { samples.Add(new Sample { at=DateTime.UtcNow.ToString("o"),elapsedMs=life.ElapsedMilliseconds,error=e.GetType().Name }); }
        }
        void ObserveExit(int wait)
        {
            if(outcome.exitObserved || child==null) return;
            try {
                if(child.HasExited || wait>0 && child.WaitForExit(wait)) {
                    outcome.exitCode=child.ExitCode;
                    outcome.exitHex="0x"+unchecked((uint)child.ExitCode).ToString("X8");
                    outcome.exitObserved=true;
                    outcome.exitedBeforeCleanup=outcome.cleanupReason==null;
                }
            } catch(InvalidOperationException) { }
        }
        public int Id { get { return child.Id; } }
        public string RequestId { get { return requestId; } }
        public QueryWorker(string executable, string root, string temporary, string runtime) : this(executable,root,temporary,runtime,false) { }
        public QueryWorker(string executable, string root, string temporary, string runtime, bool captureOwnedModules)
        {
            Directory.CreateDirectory(root); Directory.CreateDirectory(temporary);
            pipe = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.Inheritable);
            using (var parent = Process.GetCurrentProcess())
            {
                var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = root };

                start.Arguments = "--shell-menu-worker --root " + Quote(root) + " --parent " + parent.Id + " --parent-created " + parent.StartTime.ToFileTimeUtc() + " --menu-pipe " + pipe.GetClientHandleAsString();
                start.EnvironmentVariables["MAGIC_MENU_TRACE"] = "1";
                start.EnvironmentVariables["TEMP"] = temporary; start.EnvironmentVariables["TMP"] = temporary;
                if (!String.IsNullOrEmpty(runtime)) { start.EnvironmentVariables["DOTNET_ROOT"] = runtime; start.EnvironmentVariables["DOTNET_ROOT_X64"] = runtime; }
                try
                {
                    child = Process.Start(start);
                    input = new StreamWriter(child.StandardInput.BaseStream, new UTF8Encoding(false));
                    child.StandardOutput.BaseStream.CopyToAsync(Stream.Null);
                    child.StandardError.BaseStream.CopyToAsync(Stream.Null);
                    pipe.DisposeLocalCopyOfClientHandle(); reader = new StreamReader(pipe, Encoding.UTF8);
                    if(captureOwnedModules)moduleInventory=new OwnedModuleInventory(child);
                }
                catch { pipe.Dispose(); throw; }
            }
        }
        public string Open(string target, bool full, int milliseconds)
        {
            string path = String.IsNullOrWhiteSpace(target) ? "null" : JsonString(target);
            return Send("{\"op\":\"open\",\"requestId\":\"" + requestId + "\",\"target\":" + path + ",\"fullFileMenu\":" + (full ? "true" : "false") + "}", milliseconds);
        }
        public string Children(int index, int milliseconds)
        {
            if (index < 0) throw new ArgumentOutOfRangeException("index");
            return Send("{\"op\":\"children\",\"requestId\":\"" + requestId + "\",\"path\":[" + index + "]}", milliseconds);
        }
        string Send(string request, int milliseconds)
        {
            lock (sendGate)
            {
                if (unusable) throw new InvalidOperationException("Query worker is no longer usable.");
                if (milliseconds < 1 || milliseconds > 20000) throw new ArgumentOutOfRangeException("milliseconds");
                var task = Task.Run(delegate {
                    input.WriteLine(request); input.Flush();
                    return ReadBounded(reader, 2097152);
                });
                var clock=Stopwatch.StartNew();long nextSample=0;
                try {
                    while(!task.IsCompleted) {
                        if(clock.ElapsedMilliseconds>=nextSample) { SampleProcess();nextSample=clock.ElapsedMilliseconds+1000; }
                        int remaining=milliseconds-(int)clock.ElapsedMilliseconds;
                        if(remaining<=0) {
                            outcome.queryFailure="deadline";unusable=true;End("deadline");
                            // Observe a late fault after closing the owned process and response pipe.
                            task.ContinueWith(t=>{var ignored=t.Exception;},TaskContinuationOptions.OnlyOnFaulted);
                            throw new TimeoutException("Owned Shell query exceeded its deadline.");
                        }
                        ((IAsyncResult)task).AsyncWaitHandle.WaitOne(Math.Min(remaining,100));
                    }
                    string response=task.GetAwaiter().GetResult();SampleProcess(true);ObserveExit(0);return response;
                } catch(Exception e) {
                    if(outcome.queryFailure==null)outcome.queryFailure=e.GetType().Name;
                    if(!disposed) { SampleProcess(true);ObserveExit(150); }
                    throw;
                }
            }
        }
        public static string ReadBounded(TextReader reader, int limit)
        {
            var text = new StringBuilder(); int value;
            while ((value = reader.Read()) >= 0)
            {
                if (value == '\n') return text.ToString().TrimEnd('\r');
                if (text.Length >= limit) throw new InvalidDataException("Shell response exceeded the 2 MiB bound.");
                text.Append((char)value);
            }
            throw new EndOfStreamException("Shell worker exited before a complete response.");
        }
        public static string JsonString(string text)
        {
            var result = new StringBuilder("\"");
            foreach (char c in text) { if (c == '\\' || c == '"') result.Append('\\').Append(c); else if (c < 32) result.Append("\\u").Append(((int)c).ToString("x4")); else result.Append(c); }
            return result.Append('"').ToString();
        }
        public static string Quote(string text)
        {
            var result = new StringBuilder("\""); int slashes = 0;
            foreach (char c in text) { if (c == '\\') { slashes++; continue; } result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes).Append(c); slashes = 0; }
            return result.Append('\\', slashes * 2).Append('"').ToString();
        }
        void End(string reason)
        {
            if (disposed) return;
            disposed = true;
            if (child == null) return;
            ObserveExit(0);
            outcome.cleanupReason=reason;
            try
            {
                if (!unusable) {
                    try { input.Close(); } catch(IOException) { } catch(InvalidOperationException) { }
                    if(child.WaitForExit(500)) { ObserveExit(0);return; }
                }
                if (!child.HasExited) {
                    outcome.killRequested=true;
                    child.Kill(); // retained owned handle, never PID/name/tree termination
                }
                child.WaitForExit(1500);ObserveExit(0);
            }
            catch (InvalidOperationException) { ObserveExit(0); }
            finally { if (reader != null) reader.Dispose(); }
        }
        public void Dispose() { try { End("input-closed"); } finally { if (child != null) child.Dispose(); pipe.Dispose(); } }

    }
}
