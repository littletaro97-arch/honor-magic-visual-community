param([ValidateSet('Check','PrepareDestination','Secure','PrepareUser','ScanCommon','SyncUser','SyncCommon','RemoveUserData')][string]$Mode='Check',[string]$Version,[string]$ExpectedInstallRoot)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Win32Install.Core.ps1')
try {
    Assert-MagicInstallTarget $ExpectedInstallRoot
    $target=Get-MagicWin32Root
    if($Mode -eq 'PrepareUser'){
        Request-MagicInstalledUserExit
        exit (64 + (Get-MagicShortcutMask @(Get-MagicOwnedShortcuts (Get-MagicShortcutFolders))))
    }
    if($Mode -eq 'ScanCommon'){exit (64 + (Get-MagicShortcutMask @(Get-MagicOwnedShortcuts (Get-MagicShortcutFolders -Common))))}
    if($Mode -eq 'SyncUser'){Sync-MagicOwnedShortcuts (Get-MagicShortcutFolders);exit 0}
    if($Mode -eq 'SyncCommon'){Sync-MagicOwnedShortcuts (Get-MagicShortcutFolders -Common) -Common;exit 0}
    if($Mode -eq 'RemoveUserData'){
        Assert-MagicInstalledStopped
        . (Join-Path $PSScriptRoot 'Win32Install.Cleanup.ps1')
        Remove-MagicAccountData @(Get-MagicAccountProfiles) $target | Out-Null
        exit 0
    }
    # Existing installations retain their registered path. A fresh target must
    # be dedicated and below protected ancestors, before any ACL change.
    Assert-MagicInstallAncestors $target
    for($ancestor=$target;$ancestor;$ancestor=[IO.Path]::GetDirectoryName($ancestor)){
        if((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw '安装路径含链接，请先修复安装目录。'}
    }
    if($Mode -in @('Check','PrepareDestination')){
        Assert-MagicInstalledStopped
        if(Test-Path -LiteralPath $target){
            if(-not (Get-MagicRegisteredRoot) -and @(Get-ChildItem -LiteralPath $target -Force).Count){throw '首次安装请选择专用空目录，不能覆盖已有文件。'}
            $items=@(Get-MagicInstallTree $target)
            $privileged=@('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
            foreach($item in $items){
                if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw '现有安装中含链接，未覆盖。'}
                $acl=Get-Acl -LiteralPath $item.FullName
                if($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin $privileged){throw '现有安装目录所有者异常，请先检查并移走该目录，未覆盖。'}
                foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
                    if(Test-MagicUnsafeWrite $rule){throw '现有安装允许普通用户改写，请先修复目录权限，未覆盖。'}
                }
            }
            $current=Join-Path $target 'MagicCommunity.exe'
            if($Version -and (Test-Path -LiteralPath $current) -and [version]([version](Get-Item -LiteralPath $current).VersionInfo.FileVersion).ToString(3) -gt [version]$Version){throw '已安装版本较新，不能用此安装包降级。'}
        }
        Assert-MagicInstalledFilesFree
        if($Mode -eq 'PrepareDestination' -and -not (Test-Path -LiteralPath $target)){
            # Windows PowerShell creates with the final ACL before file copies.
            [void][IO.Directory]::CreateDirectory($target,(New-MagicInstallAcl))
        }
    }else{
        $admin=[Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
        $items=@(Get-MagicInstallTree $target)
        $acl=New-MagicInstallAcl
        Set-Acl -LiteralPath $target -AclObject $acl
        # Directory inheritance is reset in parent-before-child order, with no links.
        foreach($item in $items | Where-Object {$_.FullName -ine $target} | Sort-Object {$_.FullName.Length}){
            if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw '安装内含链接，权限处理已停止。'}
            $security=Get-Acl -LiteralPath $item.FullName
            $security.SetOwner($admin)
            foreach($rule in @($security.GetAccessRules($true,$false,[Security.Principal.SecurityIdentifier]))){[void]$security.RemoveAccessRuleSpecific($rule)}
            $security.SetAccessRuleProtection($false,$false)
            Set-Acl -LiteralPath $item.FullName -AclObject $security
        }
    }
    exit 0
}catch{
    Add-Type -AssemblyName System.Windows.Forms
    [void][Windows.Forms.MessageBox]::Show($_.Exception.Message,'Magic 视界社区版安装检查','OK','Warning')
    exit 32
}
