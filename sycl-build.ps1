#!/usr/bin/env pwsh

<#
.SYNOPSIS
    Build llama.cpp with the SYCL backend (Intel oneAPI) and vcpkg dependencies.

.EXAMPLE
    .\sycl-build.ps1
.EXAMPLE
    .\sycl-build.ps1 fp16 -DGGML_SYCL_DEVICE_ARCH=acm-g10
.EXAMPLE
    .\sycl-build.ps1 -DropIn
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0)]
    [ValidateSet('fp32', 'fp16')]
    [string] $Precision = 'fp32',

    [string] $BuildDir = 'build',

    # produce a layout matching the official releases: portable CPU code with all
    # runtime-selected variants, dynamically loaded backends, and RPC enabled
    [switch] $DropIn,

    [string] $VcpkgToolchain = 'C:\repo\local\vcpkg\scripts\buildsystems\vcpkg.cmake',

    [string] $VcpkgTriplet = 'x64-windows',

    [string] $OneApiSetvars,

    # NetworkDirect SDK for the RDMA transport; ND is added to the host build when present
    [string] $NdSdk = 'C:/repo/local/NetworkDirect',

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'

function Import-BatchEnvironment {
    param(
        [Parameter(Mandatory)] [string[]] $Commands
    )

    $script = Join-Path ([System.IO.Path]::GetTempPath()) ("sycl-build-env-{0}.cmd" -f [guid]::NewGuid())
    $lines = @('@echo off')
    # oneAPI setvars.bat and vcvars64.bat pushd into a directory and invoke scripts by bare
    # name, which cmd refuses to resolve while this variable is set
    $lines += 'set "NoDefaultCurrentDirectoryInExePath="'
    foreach ($c in $Commands) {
        $lines += "call $c >nul 2>&1"
        $lines += 'if errorlevel 1 exit /b 1'
    }
    $lines += 'set'

    try {
        Set-Content -Path $script -Value $lines -Encoding ASCII
        $output = & $env:ComSpec /c $script
        if ($LASTEXITCODE -ne 0) {
            throw "failed to initialize the build environment (exit code $LASTEXITCODE)"
        }
        foreach ($line in $output) {
            if ($line -match '^([^=]+)=(.*)$') {
                Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
            }
        }

        # re-running in the same session would otherwise keep appending the same entries
        $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $env:PATH = (($env:PATH -split ';' | Where-Object { $_ -and $seen.Add($_.TrimEnd('\')) }) -join ';')
    } finally {
        Remove-Item $script -Force -ErrorAction SilentlyContinue
    }
}

function Resolve-OneApiSetvars {
    if ($OneApiSetvars) {
        if (-not (Test-Path $OneApiSetvars)) {
            throw "Intel oneAPI setvars.bat not found at '$OneApiSetvars'."
        }
        return $OneApiSetvars
    }

    $candidates = @()
    if ($env:ONEAPI_ROOT) { $candidates += (Join-Path $env:ONEAPI_ROOT 'setvars.bat') }
    $candidates += @(
        (Join-Path ${env:ProgramFiles(x86)} 'Intel\oneAPI\setvars.bat')
        (Join-Path $env:ProgramFiles 'Intel\oneAPI\setvars.bat')
        'C:\Intel\oneAPI\setvars.bat'
        (Join-Path $env:USERPROFILE 'intel\oneapi\setvars.bat')
    )

    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }

    throw "Intel oneAPI setvars.bat not found. Install the Intel oneAPI Base Toolkit (see docs\backend\SYCL.md) or pass -OneApiSetvars <path>."
}

function Resolve-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found, Visual Studio does not appear to be installed."
    }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw "no Visual Studio installation with the C++ toolset was found."
    }

    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found at '$vcvars'."
    }
    return $vcvars
}

$useVcpkg = Test-Path $VcpkgToolchain
if (-not $useVcpkg) {
    Write-Warning "vcpkg toolchain not found at '$VcpkgToolchain', building without it (no OpenSSL support)."
}

$vcvars  = Resolve-VcVars
$setvars = Resolve-OneApiSetvars

Write-Host "MSVC:    $vcvars"
Write-Host "oneAPI:  $setvars"

# MSVC provides cl.exe and the Ninja shipped with Visual Studio, oneAPI provides icx
$env:NoDefaultCurrentDirectoryInExePath = $null
Import-BatchEnvironment -Commands @("`"$vcvars`"", "`"$setvars`" intel64 --force")

foreach ($tool in @('cl', 'icx', 'ninja', 'cmake')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "'$tool' was not found on PATH after initializing the build environment."
    }
}

$vcpkgArgs = @()
if ($useVcpkg) {
    $vcpkgArgs = @(
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
        "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
    )
}

$syclF16 = "-DGGML_SYCL_F16=$(if ($Precision -eq 'fp16') { 'ON' } else { 'OFF' })"

function Invoke-CMake {
    param(
        [Parameter(Mandatory)] [string]   $Dir,
        [Parameter(Mandatory)] [string[]] $Arguments,
        [string] $Target
    )

    Write-Host ""
    Write-Host "cmake -B $Dir $($Arguments -join ' ')" -ForegroundColor Cyan
    & cmake -B $Dir @Arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $buildArgs = @('--build', $Dir, '--config', 'Release', '-j')
    if ($Target) { $buildArgs += @('--target', $Target) }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $DropIn) {
    Invoke-CMake -Dir $BuildDir -Arguments (@(
        '-G', 'Ninja'
        '-DCMAKE_BUILD_TYPE=Release'
        '-DGGML_SYCL=ON'
        $syclF16
        '-DCMAKE_C_COMPILER=cl'
        '-DCMAKE_CXX_COMPILER=icx'
    ) + $vcpkgArgs + $CMakeArgs)

    Write-Host ""
    Write-Host "Build finished: $(Join-Path (Resolve-Path $BuildDir) 'bin')"
    exit 0
}

# Drop-in layout, mirroring the split used by .github/workflows/release.yml:
# the host package and the SYCL backend cannot come from one configure, because icx
# sets MSVC=TRUE and so takes the MSVC branch in ggml-cpu/CMakeLists.txt, which hand-defines
# __AVX512VNNI__ and friends instead of enabling the matching clang target features
$hostDir = "$BuildDir-host"
$syclDir = "$BuildDir-sycl"
$dist    = "$BuildDir-dist"

# the RPC backend is part of the host build, so NetworkDirect is an MSVC concern only
$ndArgs = @()
if ($NdSdk -and (Test-Path $NdSdk)) {
    Write-Host "NetworkDirect SDK: $NdSdk" -ForegroundColor Cyan
    $ndArgs = @('-DGGML_RPC_ND=ON', "-DGGML_RPC_ND_SDK=$NdSdk")
} else {
    Write-Host "NetworkDirect SDK not found, RPC will be TCP only" -ForegroundColor Yellow
}

Invoke-CMake -Dir $hostDir -Arguments (@(
    '-G', 'Ninja'
    '-DCMAKE_BUILD_TYPE=Release'
    '-DCMAKE_C_COMPILER=cl'
    '-DCMAKE_CXX_COMPILER=cl'
    '-DBUILD_SHARED_LIBS=ON'
    '-DGGML_SYCL=OFF'
    '-DGGML_NATIVE=OFF'
    '-DGGML_BACKEND_DL=ON'
    '-DGGML_CPU_ALL_VARIANTS=ON'
    '-DGGML_RPC=ON'
) + $ndArgs + $vcpkgArgs + $CMakeArgs)

Invoke-CMake -Dir $syclDir -Target 'ggml-sycl' -Arguments (@(
    '-G', 'Ninja'
    '-DCMAKE_BUILD_TYPE=Release'
    '-DCMAKE_C_COMPILER=cl'
    '-DCMAKE_CXX_COMPILER=icx'
    '-DBUILD_SHARED_LIBS=ON'
    '-DGGML_SYCL=ON'
    $syclF16
    '-DGGML_CPU=OFF'
    '-DGGML_BACKEND_DL=ON'
) + $vcpkgArgs + $CMakeArgs)

Write-Host ""
Write-Host "Assembling $dist" -ForegroundColor Cyan

Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $dist | Out-Null

Copy-Item (Join-Path $hostDir 'bin\*') $dist -Recurse -Force
Copy-Item (Join-Path $syclDir 'bin\ggml-sycl.dll') $dist -Force

if (-not $env:ONEAPI_ROOT) { throw "ONEAPI_ROOT is not set, cannot collect the oneAPI runtime." }

# runtime files the SYCL backend loads at run time, see the windows-sycl job in release.yml
$runtime = @(
    'mkl\latest\bin\mkl_sycl_blas.*.dll'
    'mkl\latest\bin\mkl_core.*.dll'
    'mkl\latest\bin\mkl_tbb_thread.*.dll'
    'compiler\latest\bin\ur_adapter_level_zero.dll'
    'compiler\latest\bin\ur_adapter_level_zero_v2.dll'
    'compiler\latest\bin\ur_adapter_opencl.dll'
    'compiler\latest\bin\ur_loader.dll'
    'compiler\latest\bin\ur_win_proxy_loader.dll'
    'compiler\latest\bin\sycl?.dll'
    'compiler\latest\bin\svml_dispmd.dll'
    'compiler\latest\bin\libmmd.dll'
    'compiler\latest\bin\libiomp5md.dll'
    'compiler\latest\bin\sycl-ls.exe'
    'compiler\latest\bin\libsycl-fallback-bfloat16.spv'
    'compiler\latest\bin\libsycl-native-bfloat16.spv'
    'dnnl\latest\bin\dnnl.dll'
    'tbb\latest\bin\tbb12.dll'
    'tcm\latest\bin\tcm.dll'
    'tcm\latest\bin\libhwloc-15.dll'
    'umf\latest\bin\umf.dll'
)

$missing = @()
foreach ($pattern in $runtime) {
    $hits = Get-ChildItem (Join-Path $env:ONEAPI_ROOT $pattern) -ErrorAction SilentlyContinue
    if ($hits) { Copy-Item $hits $dist -Force } else { $missing += $pattern }
}

if ($missing) {
    # ze_loader.dll normally comes from the installed Intel GPU driver, the Level Zero
    # SDK archive only ships headers and import libraries
    Write-Warning "not found under '$env:ONEAPI_ROOT':`n  $($missing -join "`n  ")"
}

# cl links the CPU backends against the MSVC OpenMP runtime, which is not part of a
# default Windows install the way the UCRT api-ms-win-crt-* sets are
$redist = $env:VCToolsRedistDir
if (-not $redist -or -not (Test-Path $redist)) {
    $redistRoot = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path (Split-Path (Get-Command cl).Source)))) '..') 'Redist\MSVC'
    $redist = (Get-ChildItem $redistRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } | Select-Object -Last 1).FullName
}

if ($redist) {
    $found = @()
    foreach ($sub in 'Microsoft.VC143.CRT', 'Microsoft.VC143.OpenMP') {
        $hits = Get-ChildItem (Join-Path $redist "x64\$sub\*.dll") -ErrorAction SilentlyContinue
        if ($hits) { Copy-Item $hits $dist -Force; $found += $hits.Name }
    }
    Write-Host "  MSVC runtime: $($found -join ', ')"
} else {
    Write-Warning "MSVC redistributable directory not found, VCOMP140.DLL and the CRT will have to come from the target machine."
}

Write-Host ""
Write-Host "Drop-in package: $(Resolve-Path $dist)"
Write-Host ("  {0} files" -f (Get-ChildItem $dist -File).Count)
