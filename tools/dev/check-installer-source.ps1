param([string]$SourceDirectory=(Join-Path $PSScriptRoot '../distribution'),[string]$Report)
$ErrorActionPreference='Stop'
$source=[IO.Path]::GetFullPath($SourceDirectory)
$results=@()
# Setup launches the OS Windows PowerShell, not the developer's pwsh. Keep
# source bytes unambiguous even when the customer's ANSI code page is 936/1252.
foreach($name in @('Win32Install.ps1','Win32Install.Core.ps1','Win32Install.Cleanup.ps1')){
    $path=Join-Path $source $name
    $bytes=[IO.File]::ReadAllBytes($path)
    $bom=$bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191
    if(-not $bom){throw ('Installer PowerShell source must be UTF-8 with BOM: '+$name)}
    $utf8=[Text.UTF8Encoding]::new($false,$true)
    $text=$utf8.GetString($bytes,3,$bytes.Length-3)
    $tokens=$null;$errors=$null
    [void][Management.Automation.Language.Parser]::ParseInput($text,[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ('Installer PowerShell source does not parse: '+$name+'; '+$errors[0].Message)}
    $results+=@{file=$name;bytes=$bytes.Length;encoding='utf-8-bom';sha256=(Get-FileHash -LiteralPath $path).Hash}
}
$result=[ordered]@{allPass=$true;hostVersion=$PSVersionTable.PSVersion.ToString();sourceDirectory=$source;files=$results;executedInstaller=$false}
if($Report){$result|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $Report -Encoding UTF8}
[pscustomobject]$result
