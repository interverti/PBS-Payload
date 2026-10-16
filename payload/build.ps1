# Build the PS5 streaming payload with host clang and the PS5 Payload SDK.
# Set PS5_PAYLOAD_SDK or pass -SdkPath. The build embeds browser assets.

[CmdletBinding()]
param(
  [string]$SdkPath = $env:PS5_PAYLOAD_SDK,
  [string]$Output,
  [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
  [switch]$SdkStyle,
  [switch]$TesterRelease
)

$ErrorActionPreference = 'Stop'

$viewer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\h264.html') -Raw
$muxer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\h264-mux.js') -Raw
$viewer = $viewer.Replace('/*PBS_MUX*/', $muxer)
$escapedViewer = $viewer.Replace('\', '\\').Replace('"', '\"').Replace("`r", '').Replace("`n", '\n')
$viewerHeader = '#pragma once' + "`n" + 'static const char PBS_H264_HTML[] = "' + $escapedViewer + '";' + "`n"
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot 'src\h264_webui.h'), $viewerHeader, [System.Text.UTF8Encoding]::new($false))
$bannerBytes = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'web\banner.png'))
$bannerHeader = '#pragma once' + "`n" + 'static const unsigned char PBS_BANNER_BYTES[] = {' + ($bannerBytes -join ',') + '};' + "`n"
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot 'src\h264_banner.h'), $bannerHeader, [System.Text.UTF8Encoding]::new($false))

$candidates = @(
  $SdkPath,
  (Join-Path $PSScriptRoot '..\..\sdk')
) | Where-Object { $_ -and (Test-Path (Join-Path $_ 'target\include\ps5\kernel.h')) }

$sdk = $candidates | Select-Object -First 1
if (-not $sdk) {
  throw "PS5 Payload SDK not found. Set `$env:PS5_PAYLOAD_SDK or pass -SdkPath."
}
$sdk = (Resolve-Path $sdk).Path

$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
if (-not $clang) {
  $local = Join-Path $sdk 'llvm\clang.exe'
  if (Test-Path $local) { $clang = $local }
}
if (-not $clang) { throw 'clang not found in PATH or the SDK.' }

if (-not $Output) { $Output = Join-Path $PSScriptRoot 'build\pbs.elf' }
$outDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$toolDir = if ($IsWindows -or $env:OS -eq 'Windows_NT') { Join-Path $sdk 'win' } else { Join-Path $sdk 'bin' }
if (Test-Path $toolDir) { $env:PATH = "$toolDir;$env:PATH" }

$sources = @('main.c', 'inject.c', 'h264_http.c') | ForEach-Object { Join-Path $PSScriptRoot ('src/' + $_) }

$kernelHeader = Join-Path $sdk 'target\include\ps5\kernel.h'
$sdkDefines = @()
if($TesterRelease){$sdkDefines += '-DPBS_TESTER_RELEASE'}
if ((Test-Path $kernelHeader) -and
    ((Get-Content -LiteralPath $kernelHeader -Raw) -match 'kernel_get_ucred_attrs\s*\(\s*pid_t\s+pid\s*,\s*uint8_t')) {
  $sdkDefines += '-DPBS_UCRED_ATTRS_ARRAY'
}

$cflags = if ($Config -eq 'Debug') { @('-O0', '-g3') } else { @('-O2', '-g') }

if ($SdkStyle) {
  $argsList = @(
    '--start-no-unused-arguments',
    '-target', 'x86_64-sie-ps5',
    '-isysroot', $sdk,
    '-isystem', (Join-Path $sdk 'target\include'),
    '-L', (Join-Path $sdk 'target\lib'),
    '-L', (Join-Path $sdk 'target\user\homebrew\lib'),
    '-fno-stack-protector', '-fno-plt', '-femulated-tls',
    '-lc', '-lkernel_web',
    '--end-no-unused-arguments'
  ) + $cflags + $sdkDefines + @(
    '-Wall', '-Wextra', '-Wno-unused-parameter'
  ) + $sources + @(
    '-lSceRegMgr', '-lSceUserService', '-lSceRemoteplay', '-lSceSystemService',
    '-lSceAudioOut',
    '--start-no-unused-arguments',
    (Join-Path $sdk 'target\lib\crt1.o'),
    '-lSceLibcInternal', '-lSceNet',
    '--end-no-unused-arguments',
    '-o', $Output
  )
} else {
  $argsList = @(
    '--start-no-unused-arguments',
    '-target', 'x86_64-sie-ps5',
    "--sysroot=$sdk",
    '-isystem', (Join-Path $sdk 'target\include'),
    '-L', (Join-Path $sdk 'target\lib'),
    '-L', (Join-Path $sdk 'target\user\homebrew\lib'),
    '-fno-stack-protector', '-fno-plt', '-femulated-tls',
    '-nostartfiles'
  ) + $cflags + $sdkDefines + @(
    '-Wall', '-Wextra', '-Wno-unused-parameter'
  ) + $sources + @(
    '-lc', '-lkernel_web',
    '--end-no-unused-arguments',
    (Join-Path $sdk 'target\lib\crt1.o'),
    '-lSceLibcInternal', '-lSceNet',
    '-lSceRegMgr', '-lSceUserService', '-lSceRemoteplay', '-lSceSystemService',
    '-lSceAudioOut',
    '-o', $Output
  )
}

Write-Host "sdk    : $sdk"
Write-Host "clang  : $clang"
Write-Host "config : $Config"
& $clang @argsList
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

Get-Item $Output | Select-Object FullName, Length
