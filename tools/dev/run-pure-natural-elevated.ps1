param([string]$NativeBackend)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output=Join-Path $root ('artifacts/pure-natural-elevated-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
[void][IO.Directory]::CreateDirectory($output)
$dotnet=Join-Path $root 'tools/dotnet/dotnet.exe'
$dll=Join-Path $root 'artifacts/pure-natural-publish/MagicCommunity.dll'
$report=Join-Path $output 'runtime.json'
$stdout=Join-Path $output 'stdout.log'
$stderr=Join-Path $output 'stderr.log'
$backendArgument=''
if($NativeBackend){$backendArgument=" --native-backend '$NativeBackend'"}
$inner="& '$dotnet' '$dll' --root '$root' --pure-natural-runtime-test --report '$report'$backendArgument *> '$stdout'"
$arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-Command',$inner)
try{
  $process=Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -ArgumentList $arguments -Verb RunAs -PassThru -WindowStyle Hidden
  $completed=$process.WaitForExit(120000)
  $exit=-1
  if($completed){$exit=$process.ExitCode}
  $stdoutText=''
  if(Test-Path -LiteralPath $stdout){$stdoutText=Get-Content -LiteralPath $stdout -Raw}
  $stderrText=''
  if(Test-Path -LiteralPath $stderr){$stderrText=Get-Content -LiteralPath $stderr -Raw}
  [ordered]@{ok=$completed;exitCode=$exit;output=$output;report=$report;reportExists=(Test-Path -LiteralPath $report);stdout=$stdoutText;stderr=$stderrText}|ConvertTo-Json -Depth 4
}catch{
  [ordered]@{ok=$false;error=$_.Exception.ToString();output=$output;reportExists=(Test-Path -LiteralPath $report)}|ConvertTo-Json -Depth 4
}
