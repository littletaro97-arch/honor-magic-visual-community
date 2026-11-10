param([switch]$Browser,[switch]$BuildOnly)
if($BuildOnly -and $Browser){throw "BuildOnly cannot run browser checks."}
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$runId=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$output=Join-Path $root ('artifacts/check-'+$runId);[void][IO.Directory]::CreateDirectory($output)
$env:DOTNET_ROOT=Join-Path $root 'tools/dotnet';$env:DOTNET_ROOT_X64=$env:DOTNET_ROOT
$env:DOTNET_CLI_HOME=Join-Path $root 'tools/dotnet-home';$env:DOTNET_CLI_TELEMETRY_OPTOUT='1';$env:DOTNET_NOLOGO='1';$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1'
$env:NUGET_PACKAGES=Join-Path $root 'tools/nuget-packages'
$dotnet=Join-Path $env:DOTNET_ROOT 'dotnet.exe'
if(-not(Test-Path -LiteralPath $dotnet)){throw 'Run tools/dev/setup-worktree.ps1 first.'}
function Run-Check([string]$name,[string]$program,[string[]]$arguments){
    $text=& $program @arguments 2>&1
    $code=$LASTEXITCODE
    $text|Out-File -LiteralPath (Join-Path $output ($name+'.log')) -Encoding UTF8
    if($code -ne 0){throw ($name+' failed with exit code '+$code+'; see '+$output)}
}
$oldLocation=Get-Location
try{
    Set-Location -LiteralPath $root
    Run-Check 'transition-frames' (Join-Path $PSHOME 'pwsh.exe') @('-NoProfile','-File',(Join-Path $root 'tools/dev/build-transition-frames.ps1'),'-InputWebm',(Join-Path $root 'resources/transitions/entrance-alpha.webm'),'-OutputDirectory',(Join-Path $root 'artifacts/transition-frames-hd'))
    Run-Check 'desktop-build' 'node' @('widgets/desktop/build.mjs')
    $project=Join-Path $root 'src/MagicCommunity/MagicCommunity.csproj'
    Run-Check 'restore' $dotnet @('restore',$project,'--ignore-failed-sources','--nologo')
    $app=Join-Path $output 'app'
    Run-Check 'publish' $dotnet @('publish',$project,'--no-restore','-c','Release','--self-contained','false','-o',$app,'--nologo')
    $tests=if($BuildOnly){@()}else{@('self-test','pure-natural-test','pure-natural-config-test','pure-natural-operations-test','pure-natural-schedule-test','process-guard-fixture','memory-service-fixture','pure-natural-migration-fixture','pure-natural-service-fixture','desktop-card-test','custom-card-test','desktop-refresh-test','desktop-group-test','clipboard-move-data-test')}
    $results=@()
    foreach($test in $tests){
        $report=Join-Path $output ($test+'.json')
        Run-Check $test $dotnet @((Join-Path $app 'MagicCommunity.dll'),'--root',$root,('--'+$test),'--report',$report)
        $result=Get-Content -LiteralPath $report -Raw -Encoding UTF8|ConvertFrom-Json
        if($result.allPass -ne $true){throw ($test+' did not report allPass.')}
        $results+=@{name=$test;allPass=$true;report=$report}
    }
    if($Browser){
        foreach($script in @('test_card_management64.mjs','test_card_grid65.mjs')){
            Run-Check $script 'node' @((Join-Path $root ('tools/review/'+$script)))
            $results+=@{name=$script;allPass=$true;scope='Isolated Edge with mocked native bridge'}
        }
    }
    $branch=& git branch --show-current;$commit=& git rev-parse HEAD
    $summary=@{allPass=$true;root=$root;branch=$branch;commit=$commit;tests=$results;browser=[bool]$Browser;buildOnly=[bool]$BuildOnly;output=$output;scope=$(if($BuildOnly){'Source build only; no tests or desktop processes executed.'}else{'Source build and owned fixtures only; no live desktop, startup, clipboard or user-state acceptance.'})}
    $summary|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding UTF8
    $summary|ConvertTo-Json -Depth 6
}finally{Set-Location $oldLocation}
