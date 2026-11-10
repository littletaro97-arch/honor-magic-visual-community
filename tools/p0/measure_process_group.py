"""Read-only Win32 sampler for explicit Seelen root PIDs and their descendants."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import datetime
import hashlib
import json
import os
from pathlib import Path
import time

K = C.WinDLL("kernel32", use_last_error=True)
K.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
K.CreateToolhelp32Snapshot.restype = W.HANDLE
K.CloseHandle.argtypes = [W.HANDLE]
K.CloseHandle.restype = W.BOOL
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenProcess.restype = W.HANDLE


class Entry(C.Structure):
    _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("pid", W.DWORD),
                ("heap", C.c_size_t), ("module", W.DWORD), ("threads", W.DWORD),
                ("parent", W.DWORD), ("priority", W.LONG), ("flags", W.DWORD),
                ("exe", W.WCHAR * 260)]


class Counters(C.Structure):
    _fields_ = [("size", W.DWORD), ("faults", W.DWORD)] + [
        (name, C.c_size_t) for name in ["peak_ws", "ws", "peak_paged", "paged",
                                      "peak_nonpaged", "nonpaged", "pagefile",
                                      "peak_pagefile", "private", "private_ws"]
    ] + [("shared_commit", C.c_ulonglong)]


for name in ["Process32FirstW", "Process32NextW"]:
    getattr(K, name).argtypes = [W.HANDLE, C.POINTER(Entry)]
    getattr(K, name).restype = W.BOOL
K.K32GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(Counters), W.DWORD]
K.K32GetProcessMemoryInfo.restype = W.BOOL
K.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(C.c_ulonglong)] * 4
K.GetProcessTimes.restype = W.BOOL


def processes():
    handle = K.CreateToolhelp32Snapshot(2, 0)
    if handle == W.HANDLE(-1).value:
        raise C.WinError(C.get_last_error())
    try:
        entry = Entry(size=C.sizeof(Entry))
        rows = {}
        ok = K.Process32FirstW(handle, C.byref(entry))
        while ok:
            rows[entry.pid] = {"pid": entry.pid, "parent_pid": entry.parent, "name": entry.exe}
            ok = K.Process32NextW(handle, C.byref(entry))
        error = C.get_last_error()
        if error != 18:  # ERROR_NO_MORE_FILES
            raise C.WinError(error)
        return rows
    finally:
        K.CloseHandle(handle)


def read_process(pid):
    handle = K.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
    if not handle:
        return {"status": "unavailable", "winerror": C.get_last_error()}
    try:
        memory = Counters(size=C.sizeof(Counters))
        if not K.K32GetProcessMemoryInfo(handle, C.byref(memory), memory.size):
            return {"status": "unavailable", "winerror": C.get_last_error()}
        created, exited, kernel, user = [C.c_ulonglong() for _ in range(4)]
        if not K.GetProcessTimes(handle, *[C.byref(x) for x in [created, exited, kernel, user]]):
            return {"status": "unavailable", "winerror": C.get_last_error()}
        if exited.value:
            return {"status": "exited", "creation_filetime": created.value, "exit_filetime": exited.value}
        return {"status": "observed", "creation_filetime": created.value,
                "exit_filetime": exited.value,
                "working_set_total_bytes": memory.ws, "private_bytes": memory.private,
                "private_working_set_bytes": memory.private_ws,
                "cpu_seconds": (kernel.value + user.value) / 10_000_000}
    finally:
        K.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--roots", type=int, nargs="+", required=True)
    parser.add_argument("--phase", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--samples", type=int, default=120)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.relative_to(root)
    if output.exists():
        raise FileExistsError(output)
    initial = processes()
    if any(pid not in initial for pid in args.roots):
        raise RuntimeError("Explicit root process is not running")
    root_names = {pid: initial[pid]["name"] for pid in args.roots}
    root_identities = {pid: read_process(pid).get("creation_filetime") for pid in args.roots}
    allowed_roots = ("seelen", "slu", "magiccommunity", "magicsystemnative", "magicnativeentry", "magiccompositionhost")
    if any(not Path(name).name.lower().startswith(allowed_roots) for name in root_names.values()):
        raise RuntimeError("Root PID is not a supported community/Seelen executable")
    if C.sizeof(Counters) != 96:
        raise RuntimeError("This probe requires x64 EX2 layout")
    samples, previous = [], {}
    start = time.monotonic()
    previous_elapsed = 0.0
    while len(samples) < args.samples:
        elapsed = time.monotonic() - start
        current = processes()
        selected = {pid for pid in args.roots if pid in current and current[pid]["name"] == root_names[pid]}
        while True:
            expanded = selected | {pid for pid, row in current.items() if row["parent_pid"] in selected}
            if expanded == selected:
                break
            selected = expanded
        rows, next_previous = [], {}
        cpu_delta = 0.0
        for pid in sorted(selected):
            row = current[pid] | read_process(pid)
            if pid in root_identities and root_identities[pid] is not None and row.get("creation_filetime") not in (None, root_identities[pid]):
                row = current[pid] | {"status": "identity_changed"}
            if row["status"] == "observed":
                key = (pid, row["creation_filetime"])
                if key in previous:
                    cpu_delta += max(0, row["cpu_seconds"] - previous[key])
                next_previous[key] = row["cpu_seconds"]
            rows.append(row)
        previous = next_previous
        if elapsed >= args.warmup:
            complete = bool(rows) and all(row["status"] == "observed" for row in rows) and all(pid in selected for pid in args.roots)
            samples.append({"elapsed_seconds": elapsed, "interval_seconds": elapsed - previous_elapsed,
                            "complete": complete, "processes": rows,
                            "observed_cpu_delta_seconds": cpu_delta,
                            "observed_totals": {field: sum(row.get(field, 0) for row in rows)
                                                for field in ["working_set_total_bytes", "private_bytes", "private_working_set_bytes"]}})
        previous_elapsed = elapsed
        time.sleep(max(0, 1 - (time.monotonic() - start - elapsed)))
    report = {"schema_version": 1, "captured_at": datetime.datetime.now().astimezone().isoformat(),
              "phase": args.phase, "root_pids": args.roots, "root_names": root_names,
              "root_creation_filetimes": root_identities,
              "warmup_seconds": args.warmup, "sample_count": args.samples,
              "logical_processors": os.cpu_count(), "counter_size": C.sizeof(Counters),
              "sampler_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "samples": samples,
              "limitations": ["Read-only; no process launch/stop or forced trimming.",
                              "Totals exclude unreadable processes and are incomplete when complete=false.",
                              "Descendants are determined by parent PID; detached/shared outside-tree processes require separate attribution.",
                              "No GPU measurement or manual visual/drag/drop acceptance."]}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(output), "sample_count": len(samples),
                      "all_complete": all(sample["complete"] for sample in samples)}))


if __name__ == "__main__":
    main()
