param([Parameter(Mandatory)][string]$BundleDirectory,[Parameter(Mandatory)][string]$Compiler,[Parameter(Mandatory)][string]$OutputDirectory,[ValidatePattern('^r[1-9][0-9]*$')][string]$InstallerRevision)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$bundle=[IO.Path]::GetFullPath($BundleDirectory);$out=[IO.Path]::GetFullPath($OutputDirectory)
foreach($path in @($bundle,$out)){if(-not $path.StartsWith((Join-Path $root artifacts)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Build paths must remain in this checkout artifacts directory.'}}
if(Test-Path -LiteralPath $out){throw 'Use a new output directory.'}
$manifest=Get-Content -LiteralPath (Join-Path $bundle 'setup-bundle.json') -Raw|ConvertFrom-Json
if($manifest.installKind -cne 'win32' -or $manifest.signingMode -cne 'unsigned' -or $manifest.signed -or $manifest.packageFamily -cne 'MagicCommunity.Unpackaged'){throw 'Expected an unpackaged unsigned distribution.'}
if((Get-FileHash -LiteralPath (Join-Path $bundle $manifest.packageFile)).Hash -cne $manifest.sha256){throw 'Payload archive digest differs.'}
$stage=Join-Path $bundle 'payload'
if(Test-Path -LiteralPath (Join-Path $stage 'AppxManifest.xml')){throw 'An MSIX manifest is not an unsigned EXE distribution.'}
$version=([version]$manifest.version).ToString(3)
[void][IO.Directory]::CreateDirectory($out)
$windowsPowerShell=Join-Path ([Environment]::GetFolderPath('Windows')) 'System32/WindowsPowerShell/v1.0/powershell.exe'
& $windowsPowerShell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check-installer-source.ps1') -Report (Join-Path $out 'installer-source-check.json') *> (Join-Path $out 'installer-source-check.log')
if($LASTEXITCODE -ne 0){throw ('Windows PowerShell source preflight failed; see '+(Join-Path $out 'installer-source-check.log'))}
$signature=Get-AuthenticodeSignature -LiteralPath $Compiler
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Pyrsys'){throw 'Inno Setup compiler signature is not trusted.'}
$suffix=if($InstallerRevision){'-'+$InstallerRevision}else{''}
& $Compiler ('/DPayload='+$stage) ('/DOutput='+$out) ('/DReleaseVersion='+$version) ('/DInstallerSuffix='+$suffix) (Join-Path $root 'tools/distribution/MagicCommunity.iss') *> (Join-Path $out 'compile.log')
if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $out 'compile.log') -Tail 20|Out-Host;throw 'Unsigned installer compile failed.'}
$exe=Join-Path $out ('MagicCommunity-Setup-'+$version+'-x64-unsigned'+$suffix+'.exe')
if((Get-AuthenticodeSignature -LiteralPath $exe).Status -ne 'NotSigned'){throw 'The installer must not depend on a local signing identity.'}
$files=@(Get-ChildItem -LiteralPath $stage -File -Recurse|ForEach-Object {@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
$installerSources=@('MagicCommunity.iss','Win32Install.ps1','Win32Install.Core.ps1','Win32Install.Native.cs','Win32Install.Cleanup.ps1','Win32ProfileCleanup.cs')|ForEach-Object{@{path=('tools/distribution/'+$_);sha256=(Get-FileHash -LiteralPath (Join-Path $root ('tools/distribution/'+$_))).Hash}}
$report=[ordered]@{version=$version;installerRevision=$InstallerRevision;sourceCommit=(& git -C $root rev-parse HEAD).Trim();payloadSourceCommit=$manifest.sourceCommit;installKind='win32';signingMode='unsigned';certificateStoreModified=$false;msixRequired=$false;entry=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash;bytes=(Get-Item -LiteralPath $exe).Length;compilerSha256=(Get-FileHash -LiteralPath $Compiler).Hash;compilerVersion=(Get-Item -LiteralPath $Compiler).VersionInfo.FileVersion;payloadSha256=$manifest.sha256;files=$files;installerSources=$installerSources;installed=$false;desktopActivated=$false}
$report|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $out 'installer-build.json') -Encoding UTF8
[pscustomobject]$report|Select-Object version,sourceCommit,entry,sha256,bytes,installKind,signingMode,installed|ConvertTo-Json
