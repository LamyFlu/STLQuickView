[CmdletBinding()]
param([switch]$Uninstall, [switch]$Thumbnails)

$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
    throw 'Use 64-bit PowerShell on x64 Windows.'
}
if ($Uninstall -and $Thumbnails) { throw 'Use -Uninstall or -Thumbnails separately.' }

$user = [Microsoft.Win32.Registry]::CurrentUser
$merged = [Microsoft.Win32.Registry]::ClassesRoot
$classes = 'Software\Classes'
$progId = 'StlQuickView.Model'
$preview = '{8FA42D75-2473-4D80-93BA-D73BE4A577C2}'
$thumbnail = '{7746619E-43C9-4FD6-8AAC-787E3F398AB1}'
$plateThumbnail = '{C3930DB2-AD88-4E7F-920B-47544E805C39}'
$previewType = '{8895B1C6-B41F-4C1C-A562-0D564250836F}'
$thumbnailType = '{E357FCCD-A995-4576-B01F-234630154E96}'
$previewHost = '{6D2B5079-2F0B-48DD-AB7F-97CEC514D30B}'
$previewList = 'Software\Microsoft\Windows\CurrentVersion\PreviewHandlers'
$capabilities = 'Software\StlQuickView\Capabilities'
$bindings = "SystemFileAssociations\.stl\ShellEx"
$previewBinding = "$bindings\$previewType"
$thumbnailBinding = "$bindings\$thumbnailType"
$plateThumbnailBinding = ".3mf\ShellEx\$thumbnailType"
# Preview pane for the other model formats, on the per-user extension keys.
$extraPreviewBindings = '.3mf', '.obj', '.ply' | ForEach-Object { "$_\ShellEx\$previewType" }
$installRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'StlQuickView'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'STL Quick View.lnk'

function Set-Value($path, $name, $value, $kind = 'String') {
    $key = $user.CreateSubKey($path)
    try { $key.SetValue($name, $value, $kind) } finally { $key.Dispose() }
}

function Get-Value($hive, $path, $name = '') {
    $key = $hive.OpenSubKey($path)
    if ($key) {
        try { $key.GetValue($name, $null) } finally { $key.Dispose() }
    }
}

if ((Get-Value $user 'Software\StlQuickView\Installer' 'Engine') -eq 'MSI') {
    throw 'STL Quick View is installed by MSI. Manage it in Windows Installed apps.'
}

function Remove-Value($path, $name) {
    $key = $user.OpenSubKey($path, $true)
    if ($key) {
        try { $key.DeleteValue($name, $false) } finally { $key.Dispose() }
    }
}

function Remove-Binding($binding, $clsid) {
    $path = "$classes\$binding"
    if ((Get-Value $user $path) -eq $clsid) {
        Remove-Value $path ''
        $key = $user.OpenSubKey($path)
        try { $empty = $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0 } finally { $key.Dispose() }
        if ($empty) { $user.DeleteSubKey($path, $false) }
    }
}

function Test-BindingAvailable($binding, $ownClsid) {
    $existing = Get-Value $merged $binding
    if ($existing -and $existing -ne $ownClsid) {
        throw "A different handler is already registered for $binding."
    }
}

function Register-Handler($clsid, $label, $dll) {
    Set-Value "$classes\CLSID\$clsid" '' $label
    Set-Value "$classes\CLSID\$clsid\InprocServer32" '' $dll
    Set-Value "$classes\CLSID\$clsid\InprocServer32" 'ThreadingModel' 'Apartment'
}

function Notify-Explorer {
    if (-not ('StlQuickViewSetup.Shell' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
namespace StlQuickViewSetup {
    public static class Shell {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(int eventId, uint flags, IntPtr a, IntPtr b);
    }
}
'@
    }
    [StlQuickViewSetup.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Remove-Payloads($keep) {
    if (-not (Test-Path -LiteralPath $installRoot -PathType Container)) { return }
    $resolvedRoot = (Resolve-Path -LiteralPath $installRoot).ProviderPath
    if ($resolvedRoot -ne [IO.Path]::GetFullPath($installRoot) -or
        ((Get-Item -LiteralPath $resolvedRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) { return }
    foreach ($folder in (Get-ChildItem -LiteralPath $resolvedRoot -Directory)) {
        if ($folder.Name -notmatch '^payload-[a-f0-9]{24}$' -or $folder.FullName -eq $keep -or
            ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
        try {
            foreach ($name in 'Setup.ps1', 'StlQuickView.exe', 'StlQuickViewShell.dll') {
                $file = Join-Path $folder.FullName $name
                if (Test-Path -LiteralPath $file -PathType Leaf) { Remove-Item -LiteralPath $file -Force }
            }
            if (-not (Get-ChildItem -LiteralPath $folder.FullName -Force)) {
                [IO.Directory]::Delete($folder.FullName)
            }
        } catch {
            Write-Warning "Windows is using a previous copy. Cleanup can be retried after signing out: $($folder.FullName)"
        }
    }
}

if ($Uninstall) {
    Remove-Binding $previewBinding $preview
    Remove-Binding $thumbnailBinding $thumbnail
    Remove-Binding $plateThumbnailBinding $plateThumbnail
    foreach ($binding in $extraPreviewBindings) { Remove-Binding $binding $preview }
    Remove-Value $previewList $preview
    Remove-Value 'Software\RegisteredApplications' 'STL Quick View'
    Remove-Value "$classes\.stl\OpenWithProgids" $progId
    foreach ($path in "$classes\$progId", "$classes\CLSID\$preview", "$classes\CLSID\$thumbnail", "$classes\CLSID\$plateThumbnail", $capabilities) {
        $user.DeleteSubKeyTree($path, $false)
    }
    if (Test-Path -LiteralPath $shortcut -PathType Leaf) { Remove-Item -LiteralPath $shortcut }
    Notify-Explorer
    Remove-Payloads ''
    Write-Output 'Removed STL Quick View integration for this user.'
    return
}

Test-BindingAvailable $previewBinding $preview
if ($Thumbnails) { Test-BindingAvailable $thumbnailBinding $thumbnail }
$sourceExe = Join-Path $PSScriptRoot 'StlQuickView.exe'
$sourceDll = Join-Path $PSScriptRoot 'StlQuickViewShell.dll'
$exeHash = (Get-FileHash -LiteralPath $sourceExe).Hash
$dllHash = (Get-FileHash -LiteralPath $sourceDll).Hash
$payload = Join-Path $installRoot ('payload-' + ($exeHash.Substring(0,12) + $dllHash.Substring(0,12)).ToLowerInvariant())
$null = New-Item -Path $payload -ItemType Directory -Force
$exe = Join-Path $payload 'StlQuickView.exe'
$dll = Join-Path $payload 'StlQuickViewShell.dll'
foreach ($source in $sourceExe, $sourceDll, $PSCommandPath) {
    $target = Join-Path $payload ([IO.Path]::GetFileName($source))
    if ($source -ne $target -and
        (-not (Test-Path -LiteralPath $target) -or
         (Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash)) {
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
}

Register-Handler $preview 'STL Quick View Preview' $dll
Set-Value "$classes\CLSID\$preview" 'AppID' $previewHost
Set-Value $previewList $preview 'STL Quick View Preview'

Set-Value "$classes\$progId" '' 'STL 3D Model'
Set-Value "$classes\$progId\DefaultIcon" '' ('"' + $exe + '",0')
Set-Value "$classes\$progId\shell\open\command" '' ('"' + $exe + '" "%1"')
Set-Value "$classes\$progId\ShellEx\$previewType" '' $preview
Set-Value "$classes\.stl\OpenWithProgids" $progId ''
Set-Value $capabilities 'ApplicationName' 'STL Quick View'
Set-Value $capabilities 'ApplicationDescription' 'View STL 3D models.'
Set-Value "$capabilities\FileAssociations" '.stl' $progId
Set-Value 'Software\RegisteredApplications' 'STL Quick View' $capabilities

if ($Thumbnails) {
    Register-Handler $thumbnail 'STL Quick View Thumbnail' $dll
    Set-Value "$classes\$progId\ShellEx\$thumbnailType" '' $thumbnail
    Set-Value "$classes\$thumbnailBinding" '' $thumbnail
    $existingPlateThumbnail = Get-Value $user "$classes\$plateThumbnailBinding"
    if (-not $existingPlateThumbnail -or $existingPlateThumbnail -eq $plateThumbnail) {
        Register-Handler $plateThumbnail 'STL Quick View 3MF Plate Thumbnail' $dll
        Set-Value "$classes\$plateThumbnailBinding" '' $plateThumbnail
    } else {
        Write-Warning 'A different per-user 3MF thumbnail handler is already registered. It will be retained.'
    }
} else {
    Remove-Binding $thumbnailBinding $thumbnail
    Remove-Binding "$progId\ShellEx\$thumbnailType" $thumbnail
    $user.DeleteSubKeyTree("$classes\CLSID\$thumbnail", $false)
    Remove-Binding $plateThumbnailBinding $plateThumbnail
    $user.DeleteSubKeyTree("$classes\CLSID\$plateThumbnail", $false)
}
Set-Value "$classes\$previewBinding" '' $preview
foreach ($binding in $extraPreviewBindings) {
    $existing = Get-Value $user "$classes\$binding"
    if (-not $existing -or $existing -eq $preview) {
        Set-Value "$classes\$binding" '' $preview
    } else {
        Write-Warning "A different per-user preview handler is already registered for $binding. It will be retained."
    }
}

$link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
$link.TargetPath = $exe
$link.WorkingDirectory = $payload
$link.IconLocation = "$exe,0"
$link.Save()

Notify-Explorer
Remove-Payloads $payload
Write-Output 'Installed STL Quick View and Explorer preview for this user.'
Write-Output 'Select STL Quick View in Windows Default apps to use its STL icon and open files by double-click.'
