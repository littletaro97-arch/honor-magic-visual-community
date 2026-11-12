# No work on import. The installer calls this only for an explicit delete choice.
function Get-MagicAccountProfiles {
    $machine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $list=$machine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList')
        try {
            foreach($sid in $list.GetSubKeyNames() | Where-Object {$_ -match '^S-1-(5-21|12-1)-[0-9-]+$'}) {
                $key=$list.OpenSubKey($sid)
                try {$path=[string]$key.GetValue('ProfileImagePath');if($path){[pscustomobject]@{Sid=$sid;Path=[IO.Path]::GetFullPath($path).TrimEnd('\')}}}finally{$key.Dispose()}
            }
        }finally{$list.Dispose()}
    }finally{$machine.Dispose()}
}
function Assert-MagicCleanupRoot([string]$Root,[string]$Profile,[string]$CodeRoot) {
    if($Root -notmatch '^[A-Za-z]:\\' -or $Root.Length -lt 4 -or $Root.Length -gt 200 -or $Root -cne [IO.Path]::GetFullPath($Root).TrimEnd('\')){throw '数据清理路径无效。'}
    foreach($part in $Root.Substring(3).Split('\')){if(-not $part -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0){throw '数据清理路径含无效名称。'}}
    foreach($protected in @($CodeRoot,[Environment]::GetFolderPath('Windows'),(Get-MagicProgramFiles64),[Environment]::GetFolderPath('ProgramFilesX86'),[Environment]::GetFolderPath('CommonApplicationData'))){
        if($protected -and ($Root -ieq $protected -or $Root.StartsWith($protected.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or $protected.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase))){throw '不能清理系统或安装目录。'}
    }
    foreach($base in @($Profile,(Join-Path $Profile 'Desktop'),(Join-Path $Profile 'Documents'),(Join-Path $Profile 'AppData'),(Join-Path $Profile 'AppData\Local'))){if($Root -ieq $base -or $base.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){throw '不能清理账户公共目录。'}}
    if($Root.StartsWith((Join-Path $Profile 'AppData')+'\',[StringComparison]::OrdinalIgnoreCase) -and $Root -ine (Join-Path $Profile 'AppData\Local\MagicCommunity\UserData')){throw 'AppData 中仅清理社区版默认数据目录。'}
    for($part=$Root;$part;$part=[IO.Path]::GetDirectoryName($part)){if(Test-Path -LiteralPath (Join-Path $part '.git')){throw '不能清理源码工作树。'}}
}
function Remove-MagicAccountData([object[]]$Profiles,[string]$CodeRoot) {
    if(-not ('MagicCommunity.Setup.ProfileCleanup' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'Win32ProfileCleanup.cs')}
    $guard=[MagicCommunity.Setup.ProfileCleanup]::new()
    $roots=@{};$records=[Collections.Generic.List[string]]::new();$metadata=[Collections.Generic.List[string]]::new();$directories=[Collections.Generic.List[string]]::new()
    try {
        foreach($profile in $Profiles){
            $deployment=Join-Path $profile.Path 'AppData\Local\MagicCommunity\Deployment'
            if(-not(Test-Path -LiteralPath $deployment)){continue}
            $guard.PinParents($deployment);$guard.Pin($deployment,$true,$false)
            $files=@(Get-ChildItem -LiteralPath $deployment -Force | Where-Object {$_.Name -ceq 'installation.json' -or $_.Name -cmatch '^installation\.json\.\d{17}\.bak$'} | Sort-Object Name)
            if(-not $files.Count){continue}
            foreach($file in $files){
                $guard.Pin($file.FullName,$true,$true)
                $record=$guard.ReadText($file.FullName)|ConvertFrom-Json
                if($record.protocol -cne 'Magic.Install/1' -or $record.ownerSid -cne $profile.Sid -or $record.installId -cnotmatch '^[0-9a-fA-F]{32}$' -or $record.packageFamily -cnotin @('MagicCommunity.Unpackaged','MagicCommunity.Native_x52pfh0dth434')){throw ('账户安装记录归属无法核验：'+$file.FullName)}
                $root=[string]$record.dataRoot
                Assert-MagicCleanupRoot $root $profile.Path $CodeRoot
                if(-not(Test-Path -LiteralPath ([IO.Path]::GetPathRoot($root)))){throw ('数据磁盘不可用，无法完成清理：'+$root)}
                $records.Add($file.FullName)
                if($roots.ContainsKey($root)){if($roots[$root].Sid -cne $profile.Sid -or ($file.Name -ceq 'installation.json' -and $roots[$root].Id -cne $record.installId)){throw '多个账户的数据记录冲突。'};continue}
                if(-not(Test-Path -LiteralPath $root)){continue}
                $guard.PinParents($root);$guard.Pin($root,$true,$false)
                if([MagicCommunity.Setup.ProfileCleanup]::UsesFilePermissions($root) -and (Get-Acl -LiteralPath $root).GetOwner([Security.Principal.SecurityIdentifier]).Value -cne $profile.Sid){throw ('数据目录不由记录中的账户拥有：'+$root)}
                $marker=Join-Path $root '.magic-data.json'
                if(-not(Test-Path -LiteralPath $marker)){throw ('目录存在但数据标记缺失，未删除其中的文件：'+$root+'。可以选择保留数据继续卸载。')}
                $guard.Pin($marker,$true,$true);$owner=$guard.ReadText($marker)|ConvertFrom-Json
                if($owner.protocol -cne 'Magic.Install/1' -or $owner.ownerSid -cne $profile.Sid -or $owner.installId -cne $record.installId -or $owner.dataRoot -ine $root){throw ('数据归属标记不匹配：'+$root)}
                # App-owned subtrees only. Physical desktop/collected originals
                # are stored elsewhere and are never traversal targets here.
                $trees=@('data','runtime','logs','feedback','handoff','cards' | ForEach-Object {Join-Path $root $_} | Where-Object {Test-Path -LiteralPath $_})
                foreach($tree in $trees){$guard.PlanTree($tree)}
                $roots[$root]=[pscustomobject]@{Sid=$profile.Sid;Id=$record.installId;Marker=$marker;Trees=$trees}
            }
            $receipt=Join-Path $deployment 'desktop-shortcut.json'
            if(Test-Path -LiteralPath $receipt){
                $guard.Pin($receipt,$true,$true);$value=$guard.ReadText($receipt)|ConvertFrom-Json
                if($value.protocol -cne 'Magic.DesktopShortcut/1' -or $value.ownerSid -cne $profile.Sid){throw '旧快捷方式记录归属无法核验。'}
                $metadata.Add($receipt)
            }
            $directories.Add($deployment)
        }
        # Every account/marker/tree is checked before the first deletion. Keep
        # registration until owned content has been removed. Deletion is not a
        # filesystem transaction: later I/O failure aborts uninstall, and the
        # user can choose preservation if ownership can no longer be verified.
        foreach($root in $roots.Keys){foreach($tree in $roots[$root].Trees){$guard.DeleteTree($tree)}}
        foreach($root in $roots.Keys){$guard.Delete($roots[$root].Marker);if(-not @(Get-ChildItem -LiteralPath $root -Force).Count){$guard.Delete($root)}}
        foreach($path in $metadata){$guard.Delete($path)}
        foreach($path in $records){$guard.Delete($path)}
        foreach($path in $directories){if(-not @(Get-ChildItem -LiteralPath $path -Force).Count){$guard.Delete($path)}}
        [pscustomobject]@{Accounts=$directories.Count;DataDirectories=$roots.Count;RegistrationFiles=$records.Count}
    }finally{$guard.Dispose()}
}
