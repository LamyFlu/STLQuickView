param(
    [string]$WixPath,
    # Raise the version in VERSION before building, e.g. -Bump Patch turns 1.1.0 into 1.1.1.
    [ValidateSet('Major', 'Minor', 'Patch')][string]$Bump
)

$ErrorActionPreference = 'Stop'
if (-not $WixPath) {
    $localWix = Join-Path $PSScriptRoot '..\build-tools\wix\wix.exe'
    $WixPath = if (Test-Path -LiteralPath $localWix) { $localWix } else { (Get-Command wix).Source }
}
$WixPath = (Get-Item -LiteralPath $WixPath).FullName

$versionFile = Join-Path $PSScriptRoot 'VERSION'
$version = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim()
if ($Bump) {
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must look like 1.2.3 (found '$version')." }
    [int[]]$v = $version.Split('.')
    switch ($Bump) {
        'Major' { $v = @(($v[0] + 1), 0, 0) }
        'Minor' { $v = @($v[0], ($v[1] + 1), 0) }
        'Patch' { $v = @($v[0], $v[1], ($v[2] + 1)) }
    }
    $version = $v -join '.'
    Set-Content -LiteralPath $versionFile -Value $version -Encoding ascii
    Write-Host "VERSION -> $version"
}

& (Join-Path $PSScriptRoot 'build.ps1') -Installer
Push-Location $PSScriptRoot
try {
    $wixArgs = @(
        'build', '-arch', 'x64', '-ext', 'WixToolset.UI.wixext', '-culture', 'zh-TW',
        '-pdbtype', 'none', '-intermediatefolder', 'build\wix', '-d', "Version=$version",
        '-out', 'dist\StlQuickView-win-x64.msi', 'installer\Package.wxs'
    )
    & $WixPath @wixArgs
    if ($LASTEXITCODE -ne 0) { throw "MSI build failed ($LASTEXITCODE)." }
    $zip = 'dist\StlQuickView-win-x64.zip'
    $portableFiles = @(
        'dist\StlQuickView.exe', 'dist\StlQuickViewShell.dll', 'dist\Setup.ps1', 'dist\README.md',
        'LICENSE', 'THIRD_PARTY_NOTICES.md'
    )
    Compress-Archive -Force -DestinationPath $zip -LiteralPath $portableFiles
} finally {
    Pop-Location
}
Write-Host "Built dist\StlQuickView-win-x64.msi and .zip, version $version"
