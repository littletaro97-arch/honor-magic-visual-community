using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;

namespace MagicMenuDiagnostic
{
    // Ordinary module enumeration of our retained query child. No debugger attachment,
    // process suspension, injected code, raw-memory capture or production process lookup.
    public sealed class OwnedModuleInventory
    {
        public sealed class Module
        {
            public string file, version, company, firstSeen, lastSeen, metadataError;
            public long firstSeenMs, lastSeenMs;
            public int imageBytes;
        }
        public sealed class Result
        {
            public string method="process-modules", lastError;
            public bool debuggerAttached=false, truncated;
            public int attempts, successfulSnapshots, maximumSnapshots=48, maximumModules=256, intervalMs=1000;
            public long totalCostMs, maximumCostMs;
            public Module[] modules;
        }
        readonly Process child;
        readonly Result state=new Result();
        readonly Dictionary<string,Module> modules=new Dictionary<string,Module>(StringComparer.OrdinalIgnoreCase);
        long nextAt;
        internal OwnedModuleInventory(Process ownedChild) { child=ownedChild; }
        public Result Evidence
        {
            get { state.modules=new List<Module>(modules.Values).ToArray();return state; }
        }
        internal void Capture(long elapsed,bool final=false)
        {
            if((!final && elapsed<nextAt) || state.attempts>=state.maximumSnapshots)return;
            nextAt=elapsed+state.intervalMs;state.attempts++;
            var cost=Stopwatch.StartNew();
            try {
                child.Refresh();if(child.HasExited)return;
                string at=DateTime.UtcNow.ToString("o");int count=0;
                foreach(ProcessModule loaded in child.Modules) {
                    if(++count>512) { state.truncated=true;break; }
                    string file=Path.GetFileName(loaded.ModuleName);
                    // This key is private and only disambiguates same-name loaded images.
                    string key=loaded.FileName;Module item;
                    if(!modules.TryGetValue(key,out item)) {
                        if(modules.Count>=state.maximumModules){state.truncated=true;continue;}
                        item=new Module{file=file,firstSeen=at,firstSeenMs=elapsed,imageBytes=loaded.ModuleMemorySize};
                        modules.Add(key,item);
                        try {
                            // Do not fetch version resources from UNC or mapped-network locations.
                            if(key.Length<3 || key[1]!=':' || key[2]!='\\' || new DriveInfo(Path.GetPathRoot(key)).DriveType!=DriveType.Fixed)
                                item.metadataError="non-local-metadata-skipped";
                            else {
                                var v=loaded.FileVersionInfo;
                                item.version=Bound(v.FileVersion,96);item.company=Bound(v.CompanyName,160);
                            }
                        } catch(Exception error) { item.metadataError=error.GetType().Name; }
                    }
                    item.lastSeen=at;item.lastSeenMs=elapsed;
                }
                state.successfulSnapshots++;
            } catch(Exception error) { state.lastError=error.GetType().Name; }
            finally { cost.Stop();state.totalCostMs+=cost.ElapsedMilliseconds;state.maximumCostMs=Math.Max(state.maximumCostMs,cost.ElapsedMilliseconds); }
        }
        static string Bound(string value,int limit)
        {
            if(value==null)return null;
            // Version resources are untrusted display metadata; do not export embedded paths.
            if(value.IndexOf('\\')>=0 || value.IndexOf(":/")>=0)return "<metadata-path-omitted>";
            value=value.Replace("\r"," ").Replace("\n"," ");
            return value.Length>limit?value.Substring(0,limit):value;
        }
    }
}
