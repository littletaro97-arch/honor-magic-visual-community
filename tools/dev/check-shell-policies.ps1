param(
    [string]$Compiler = 'cl.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'artifacts/shell-policy-checks' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source = Join-Path $PSScriptRoot 'shell-policy/shell-policy-probe.cpp'
$exe = Join-Path $out 'ShellPolicyProbe.exe'
$buildLog = Join-Path $out 'build.log'
Push-Location $out
try {
    & $Compiler /nologo /std:c++20 /W4 /O2 /MT /utf-8 "/Fo$(Join-Path $out 'shell-policy.obj')" "/Fe$exe" $source *> $buildLog
    if ($LASTEXITCODE -ne 0) { Get-Content $buildLog | Out-Host; throw 'Shell policy probe failed to build.' }
    $report = & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Shell policy probe failed.' }
    $result = $report | ConvertFrom-Json
    if (-not $result.allPass) { throw 'Shell policy checks did not pass.' }
    $report | Set-Content -LiteralPath (Join-Path $out 'checks.json') -Encoding UTF8
    $report
} finally { Pop-Location }
