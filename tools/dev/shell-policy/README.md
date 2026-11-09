# Shell policy regression checks

Run from a Visual Studio x64 Developer PowerShell with the C++ toolchain available:

```powershell
./tools/dev/check-shell-policies.ps1
```

`-Compiler` selects another compatible `cl.exe`; `-OutputDirectory` changes the default `artifacts/shell-policy-checks` output directory. The probe checks notification status hint suppression and exact Start menu application identity filtering without opening windows, reading user configuration, or requesting notification access.

The existing `./tools/dev/check-native-system.ps1` additionally exercises DWM cloak/uncloak events with its own test window. Neither check takes over the desktop or changes notification permissions.
