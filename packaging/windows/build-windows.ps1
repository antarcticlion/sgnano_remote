# sgnano_remote の Windows ビルド + NSIS インストーラ（2026-11-15）
# Windows 10/11 x64、Qt 6.2 以降（MinGW または MSVC）向け。
#
# 例:
#   powershell -ExecutionPolicy Bypass -File packaging\windows\build-windows.ps1 `
#       -QtDir C:\Qt\6.2.4\mingw_64
#
# makensis.exe と Qt の windeployqt.exe が PATH か QtDir の下に必要です。

param(
    [string]$QtDir = "C:\Qt\6.2.4\mingw_64",
    [string]$MinGWPath = "C:\Qt\Tools\mingw1120_64\bin",
    [string]$BuildDir = "",
    [string]$Generator = "Ninja",
    [string]$Version = "0.1.0",
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"
if (-not [string]::IsNullOrWhiteSpace($MinGWPath) -and (Test-Path $MinGWPath)) {
    $env:Path = "$MinGWPath;$env:Path"
}
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $Root "build-windows"
}
$StageDir = Join-Path $BuildDir "stage"

Write-Host "root:  $Root"
Write-Host "Qt:    $QtDir"
Write-Host "build: $BuildDir"

cmake -S $Root -B $BuildDir -G $Generator `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH=$QtDir
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (Test-Path $StageDir) {
    Remove-Item -Recurse -Force $StageDir
}
cmake --install $BuildDir --prefix $StageDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake install failed" }

$Exe = Join-Path $StageDir "sgnano_remote.exe"
if (-not (Test-Path $Exe)) { throw "sgnano_remote.exe が stage にありません: $Exe" }

$DeployQt = Join-Path $QtDir "bin\windeployqt.exe"
if (-not (Test-Path $DeployQt)) {
    $DeployQt = (Get-Command windeployqt.exe -ErrorAction SilentlyContinue).Source
}
if (-not $DeployQt) { throw "windeployqt.exe が見つかりません" }

& $DeployQt --release --compiler-runtime --no-translations --dir $StageDir $Exe
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# windeployqt が別の MinGW の実行時 DLL を拾うことがあるので、Qt と同じ GCC 11.2 のものにそろえる
foreach ($dll in @("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")) {
    $src = Join-Path $QtDir "bin\$dll"
    if (Test-Path $src) {
        Copy-Item -Force $src (Join-Path $StageDir $dll)
    }
}

if ($SkipInstaller) {
    Write-Host "インストーラを作らずに終わります: $StageDir"
    exit 0
}

$MakeNsis = (Get-Command makensis.exe -ErrorAction SilentlyContinue).Source
if (-not $MakeNsis) { throw "makensis.exe が見つかりません（NSIS を入れて PATH に通してください）" }

Push-Location $PSScriptRoot
try {
    & $MakeNsis "/DSTAGE=$StageDir" "/DVERSION=$Version" "installer.nsi"
    if ($LASTEXITCODE -ne 0) { throw "makensis failed" }
    $Installer = Join-Path $PSScriptRoot "sgnano_remote_setup_$Version.exe"
    if (-not (Test-Path $Installer)) { throw "インストーラができませんでした: $Installer" }
    Copy-Item -Force $Installer (Join-Path $BuildDir (Split-Path $Installer -Leaf))
    Write-Host "完成: $(Join-Path $BuildDir (Split-Path $Installer -Leaf))"
} finally {
    Pop-Location
}
