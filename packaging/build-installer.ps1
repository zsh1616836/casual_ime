[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [switch]$SkipBuild,
    [switch]$RequireClean,
    [string]$Build64 = 'build',
    [string]$Build32 = 'build32',
    [ValidatePattern('^[-A-Za-z0-9]*$')]
    [string]$OutputSuffix = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$versionPath = Join-Path $PSScriptRoot 'version.txt'
$manifestPath = Join-Path $PSScriptRoot 'release-assets.json'
$issPath = Join-Path $PSScriptRoot 'zime.iss'
$build64Root = [IO.Path]::GetFullPath((Join-Path $repoRoot $Build64))
$build32Root = [IO.Path]::GetFullPath((Join-Path $repoRoot $Build32))

function Invoke-Checked {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

function Assert-FileMatchesManifest {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] $Entry
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Release asset is missing: $Path"
    }
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -ne [long]$Entry.size) {
        throw "Release asset size mismatch: $Path"
    }
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne ([string]$Entry.sha256).ToLowerInvariant()) {
        throw "Release asset hash mismatch: $Path"
    }
}

function Get-PeMachine {
    param([Parameter(Mandatory)] [string]$Path)

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a PE file: $Path"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Invalid PE signature: $Path"
        }
        return $reader.ReadUInt16()
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-PeMachine {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [UInt16]$Expected
    )

    $actual = Get-PeMachine -Path $Path
    if ($actual -ne $Expected) {
        throw ('Unexpected PE architecture for {0}: expected 0x{1:X4}, got 0x{2:X4}' -f $Path, $Expected, $actual)
    }
}

function Find-Iscc {
    if ($env:ISCC -and (Test-Path -LiteralPath $env:ISCC -PathType Leaf)) {
        return [IO.Path]::GetFullPath($env:ISCC)
    }

    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }

    $uninstallRoots = @(
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($entry in Get-ItemProperty $uninstallRoots -ErrorAction SilentlyContinue) {
        if ($entry.DisplayName -like 'Inno Setup 7*' -and $entry.InstallLocation) {
            $candidate = Join-Path $entry.InstallLocation 'ISCC.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return [IO.Path]::GetFullPath($candidate)
            }
        }
    }

    throw 'Inno Setup 7 ISCC.exe was not found. Set the ISCC environment variable to its full path.'
}

$version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
if ($version -notmatch '^\d+\.\d+(?:\.\d+)?$') {
    throw "Invalid installer version: $version"
}
$imeConfig = Get-Content -LiteralPath (Join-Path $repoRoot 'ime\ime_config.h') -Raw
if ($imeConfig -notmatch '#define\s+IME_VERSION\s+L"([^\"]+)"' -or $Matches[1] -ne $version) {
    throw 'packaging\version.txt and IME_VERSION must contain the same version.'
}

$gitStatus = @(& git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to read Git status.'
}
$isDirty = $gitStatus.Count -gt 0
if ($RequireClean -and $isDirty) {
    throw 'The Git working tree is dirty. Commit the intended release changes or omit -RequireClean.'
}
$gitCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to read the Git commit.'
}

if (-not $SkipBuild) {
    Invoke-Checked -FilePath 'cmake' -ArgumentList @('--build', $build64Root, '--config', $Configuration)
    Invoke-Checked -FilePath 'cmake' -ArgumentList @('--build', $build32Root, '--config', $Configuration)
}

$x64Dll = Join-Path $build64Root "ime\$Configuration\casual_ime.dll"
$x86Dll = Join-Path $build32Root "ime\$Configuration\casual_ime.dll"
$brokerExe = Join-Path $build64Root "broker\$Configuration\zime_broker.exe"
$x64Registrar = Join-Path $build64Root "installer\$Configuration\zime_registrar.exe"
$x86Registrar = Join-Path $build32Root "installer\$Configuration\zime_registrar.exe"
foreach ($artifact in @($x64Dll, $x86Dll, $brokerExe, $x64Registrar, $x86Registrar)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Build artifact is missing: $artifact"
    }
}

Assert-PeMachine -Path $x64Dll -Expected 0x8664
Assert-PeMachine -Path $brokerExe -Expected 0x8664
Assert-PeMachine -Path $x86Dll -Expected 0x014C
Assert-PeMachine -Path $x64Registrar -Expected 0x8664
Assert-PeMachine -Path $x86Registrar -Expected 0x014C

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$dictionarySource = Join-Path $repoRoot $manifest.base_dictionary.path
Assert-FileMatchesManifest -Path $dictionarySource -Entry $manifest.base_dictionary
$installerIconSource = Join-Path $repoRoot $manifest.installer_icon.path
Assert-FileMatchesManifest -Path $installerIconSource -Entry $manifest.installer_icon
$defaultConfigSource = Join-Path $repoRoot $manifest.default_config.path
Assert-FileMatchesManifest -Path $defaultConfigSource -Entry $manifest.default_config
$runtimeIconEntries = @($manifest.runtime_icons)
if ($runtimeIconEntries.Count -ne 8) {
    throw "Exactly eight runtime status icons are required; manifest contains $($runtimeIconEntries.Count)."
}
$runtimeIconDir = Join-Path $repoRoot 'assets\status-icons'
$expectedIconNames = @($runtimeIconEntries | ForEach-Object { Split-Path $_.path -Leaf } | Sort-Object)
$actualIconNames = @(Get-ChildItem -LiteralPath $runtimeIconDir -Filter '*.png' -File | ForEach-Object Name | Sort-Object)
$iconSetDifference = @(Compare-Object -ReferenceObject $expectedIconNames -DifferenceObject $actualIconNames)
if ($iconSetDifference.Count -ne 0) {
    throw 'assets\status-icons must contain exactly the PNG files declared in release-assets.json.'
}
foreach ($icon in $manifest.runtime_icons) {
    Assert-FileMatchesManifest -Path (Join-Path $repoRoot $icon.path) -Entry $icon
}

$outRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out\installer'))
$stageDir = [IO.Path]::GetFullPath((Join-Path $outRoot "stage-$version$OutputSuffix"))
if (-not $stageDir.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe staging directory: $stageDir"
}
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
$iconStageDir = Join-Path $stageDir 'ico'
New-Item -ItemType Directory -Path $iconStageDir -Force | Out-Null

Copy-Item -LiteralPath $x64Dll -Destination (Join-Path $stageDir 'casual_ime.dll')
Copy-Item -LiteralPath $x86Dll -Destination (Join-Path $stageDir 'casual_ime32.dll')
Copy-Item -LiteralPath $brokerExe -Destination (Join-Path $stageDir 'zime_broker.exe')
Copy-Item -LiteralPath $x64Registrar -Destination (Join-Path $stageDir 'zime_registrar.exe')
Copy-Item -LiteralPath $x86Registrar -Destination (Join-Path $stageDir 'zime_registrar32.exe')
Copy-Item -LiteralPath $dictionarySource -Destination (Join-Path $stageDir 'dict.idx')
Copy-Item -LiteralPath $installerIconSource -Destination (Join-Path $stageDir 'zime.ico')
Copy-Item -LiteralPath $defaultConfigSource -Destination (Join-Path $stageDir 'default-config.ini')
foreach ($icon in $manifest.runtime_icons) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $icon.path) -Destination $iconStageDir
}

$distDir = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
$releaseBaseName = "casual_ime-$version$OutputSuffix-x64"
$installerPath = Join-Path $distDir "$releaseBaseName.exe"
Remove-Item -LiteralPath $installerPath -Force -ErrorAction SilentlyContinue

$iscc = Find-Iscc
$numericVersion = "$version.0"
Invoke-Checked -FilePath $iscc -ArgumentList @(
    "/DAppVersion=$version",
    "/DAppVersionNumeric=$numericVersion",
    "/DStageDir=$stageDir",
    "/DOutputDir=$distDir",
    "/DOutputBase=$releaseBaseName",
    $issPath
)

if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Inno Setup did not produce the expected installer: $installerPath"
}

$installer = Get-Item -LiteralPath $installerPath
$installerHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
$buildInfo = [ordered]@{
    version = $version
    configuration = $Configuration
    git_commit = $gitCommit
    git_dirty = $isDirty
    created_utc = [DateTime]::UtcNow.ToString('o')
    installer = $installer.Name
    installer_size = $installer.Length
    installer_sha256 = $installerHash
    dictionary_sha256 = ([string]$manifest.base_dictionary.sha256).ToLowerInvariant()
    default_config_sha256 = ([string]$manifest.default_config.sha256).ToLowerInvariant()
}
$buildInfo | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $distDir "$releaseBaseName.build.json") -Encoding utf8
"$installerHash  $($installer.Name)" | Set-Content -LiteralPath (Join-Path $distDir "$releaseBaseName.sha256") -Encoding ascii

Write-Host ''
Write-Host "Installer: $installerPath"
Write-Host "SHA256:   $installerHash"
Write-Host "Size:     $($installer.Length) bytes"
