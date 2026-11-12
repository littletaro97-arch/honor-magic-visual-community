param([string]$OutputDirectory='')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source=Join-Path $repo 'tools\diagnostics\context-menu'
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repo ('artifacts\deliveries\right-click-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss'))}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(-not $output.StartsWith((Join-Path $repo 'artifacts')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Delivery must remain under this worktree artifacts.'}
$stage=Join-Path $output 'Magic-RightClick-Diagnostics-v0.4.10-r4'
if(Test-Path -LiteralPath $stage){throw 'Use a new delivery directory.'}
[void][IO.Directory]::CreateDirectory($stage)
$names=@('01-右键慢或无响应.cmd','02-Windows菜单缺失.cmd','03-快捷方式与子进程诊断.cmd','04-无调试器模块诊断.cmd','Collect-ContextMenu.ps1','MenuDiagnostic.Core.ps1','MenuDiagnostic.Native.cs','MenuDiagnostic.Modules.cs','使用说明.txt')
foreach($name in $names){Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $stage $name)}
$hashes=@(foreach($name in $names){((Get-FileHash -LiteralPath (Join-Path $stage $name) -Algorithm SHA256).Hash+'  '+$name)})
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'),[string[]]$hashes,[Text.UTF8Encoding]::new($false))
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=Join-Path $output 'Magic-RightClick-Diagnostics-v0.4.10-r4.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($stage,$zip,[IO.Compression.CompressionLevel]::Optimal,$true)
@{zip=$zip;bytes=(Get-Item -LiteralPath $zip).Length;sha256=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash;source=(git -C $repo rev-parse HEAD);scope='Standalone diagnosis; no product binaries, credentials, logs or user data.'}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'delivery.json') -Encoding utf8
Get-Content -LiteralPath (Join-Path $output 'delivery.json') -Raw
