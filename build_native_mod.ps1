# build_native_mod.ps1 — Build wwhd_online.dll using MSVC (VS 2022 Build Tools)
#
# Usage:
#   .\build_native_mod.ps1 [path\to\WindWakerHD-vX.Y.Z-windows-x86_64]
#
# Requires: VS 2022 Build Tools with C++ workload (cl.exe / link.exe)
# Output: wwhd_online.dll — copy alongside manifest.json to your WWHD mods\ folder

param(
    [string]$SDK = "D:\downloads\WindWakerHD-v0.5.1-devel.23159601-windows-x86_64"
)
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# ── Locate MSVC ────────────────────────────────────────────────────────────────
$vcvarsall = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsall)) {
    # Try vswhere
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -property installationPath
        $vcvarsall = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
    }
}
if (-not (Test-Path $vcvarsall)) {
    Write-Error "Could not find vcvarsall.bat. Install VS 2022 Build Tools (C++ workload)."
    exit 1
}

# Import MSVC environment into this PowerShell session
Write-Host "Setting up MSVC environment..."
$envLines = cmd /c "`"$vcvarsall`" x64 2>nul && set"
foreach ($line in $envLines) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}

$cl   = "cl.exe"
$link = "link.exe"
$lib  = "lib.exe"

# ── Paths ──────────────────────────────────────────────────────────────────────
$NetSrc   = "$ScriptDir\runtime-ext\src\net"
$ModSrc   = "$ScriptDir\runtime-ext\src\native_mod"
$ModInc   = "$ScriptDir\runtime-ext\include"
$SdkInc   = "$SDK\sdk\include"
$EnetDir  = "$ScriptDir\enet"

# ── ENet ──────────────────────────────────────────────────────────────────────
if (-not (Test-Path $EnetDir)) {
    Write-Host "Cloning ENet 1.3.17..."
    git clone --depth 1 --branch v1.3.17 https://github.com/lsalzman/enet.git $EnetDir
}

$EnetLib = "$EnetDir\build\enet.lib"
if (-not (Test-Path $EnetLib)) {
    Write-Host "Compiling ENet..."
    $enetBuild = "$EnetDir\build"
    New-Item -ItemType Directory -Force $enetBuild | Out-Null

    $enetSrcs = @("callbacks","compress","host","list","packet","peer","protocol","win32")
    $enetObjs = @()
    foreach ($f in $enetSrcs) {
        $src = "$EnetDir\$f.c"
        $obj = "$enetBuild\$f.obj"
        & $cl /nologo /c /O2 /W0 /D_WIN32 /I"$EnetDir\include" /Fo"$obj" $src
        if ($LASTEXITCODE -ne 0) { Write-Error "ENet compile failed: $f.c"; exit 1 }
        $enetObjs += $obj
    }
    & $lib /nologo /OUT:"$EnetLib" @enetObjs
    if ($LASTEXITCODE -ne 0) { Write-Error "ENet lib failed"; exit 1 }
}

# ── Compile sources ────────────────────────────────────────────────────────────
$includes = "/I`"$ModInc`" /I`"$NetSrc`" /I`"$EnetDir\include`" /I`"$SdkInc`""
$cxxflags  = "/std:c++17 /O2 /W3 /EHsc /D_WIN32 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS"

$srcs = @(
    "$ModSrc\net_mod.cpp",
    "$NetSrc\net.cpp",
    "$NetSrc\session.cpp",
    "$NetSrc\stun.cpp",
    "$NetSrc\p2p.cpp"
)

Write-Host "Compiling WWHD Online native mod..."
$objs = @()
foreach ($src in $srcs) {
    $obj = [System.IO.Path]::ChangeExtension($src, ".obj")
    Write-Host "  $src"
    $clArgs = "$cxxflags $includes /c `"$src`" /Fo`"$obj`""
    cmd /c "$cl $clArgs"
    if ($LASTEXITCODE -ne 0) { Write-Error "Compile failed: $src"; exit 1 }
    $objs += "`"$obj`""
}

# ── Link DLL ──────────────────────────────────────────────────────────────────
Write-Host "Linking wwhd_online.dll..."
$outDll = "$ScriptDir\wwhd_online.dll"
$outLib = "$ScriptDir\wwhd_online.lib"
$objList = $objs -join " "
$linkArgs = "/nologo /DLL /OUT:`"$outDll`" /IMPLIB:`"$outLib`" $objList `"$EnetLib`" ws2_32.lib winmm.lib advapi32.lib crypt32.lib winhttp.lib user32.lib gdi32.lib"
cmd /c "$link $linkArgs"
if ($LASTEXITCODE -ne 0) { Write-Error "Link failed"; exit 1 }

Write-Host ""
Write-Host "Build successful: wwhd_online.dll"
Write-Host ""
Write-Host "Install:"
Write-Host "  1. Copy wwhd_online.dll and manifest.json to your WWHD mods\ folder."
Write-Host "  2. Launch with --online-* args.  Examples:"
Write-Host ""
Write-Host "  P2P host:"
Write-Host "    `"Wind Waker HD.exe`" --online-p2p-create https://wwhd-signal.wwhdmm.workers.dev"
Write-Host ""
Write-Host "  P2P join:"
Write-Host "    `"Wind Waker HD.exe`" --online-p2p-join https://wwhd-signal.wwhdmm.workers.dev ABCXYZ"
