# Build the PS5 streaming payload with host clang and the PS5 Payload SDK.
# Set PS5_PAYLOAD_SDK or pass -SdkPath. The build embeds browser assets.

[CmdletBinding()]
param(
  [ValidateSet('PS5', 'PS4')][string]$Platform = 'PS5',
  [ValidateSet('stream', 'probe', 'smoke', 'worker-probe')][string]$Kind = 'stream',
  [string]$WslDistribution = 'Debian',
  [string]$SdkPath,
  [string]$Output,
  [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
  [ValidateSet(0, 1)][int]$Ps4Usage = 0,
  [ValidateSet('720p', '1080p')][string]$Ps4Resolution = '720p',
  [ValidateSet(30, 60)][int]$Ps4FrameRate = 60,
  [switch]$Ps4AdmissionProbe,
  [switch]$Ps4LatchBypass,
  [switch]$Ps4BudgetUnlock,
  [switch]$Ps4DaemonDump,
  [switch]$Ps4EncoderState,
  [switch]$Ps4LowMemory,
  [switch]$Ps4Remote1080,
  [switch]$Ps4VencJournal,
  [switch]$Ps4NativeSource,
  [ValidateSet(3,4,5)][int]$Ps4MemoryCount = 3,
  [switch]$Ps4ImagePaste,
  [switch]$Ps4NativeReadWait,
  [switch]$Ps4Observers,
  [ValidateSet('auto', 'remote', 'local')][string]$Ps4Avcap = 'auto',
  [switch]$Ps4RemoteLifecycle,
  [switch]$Ps4RemoteIdleRun,
  [ValidateSet(-1, 0, 1)][int]$Ps4WorkerBoot = -1,
  [ValidateRange(1, 3600)][int]$Ps4CaptureSeconds = 10,
  [switch]$SdkStyle,
  [switch]$TesterRelease,
  [switch]$Ps5NoNativeRateDefault
)

$ErrorActionPreference = 'Stop'

if ($Platform -eq 'PS4') {
  if ($Output) { throw 'PS4 outputs are written to payload/build/ps4. Omit -Output.' }
  if (-not $SdkPath) { $SdkPath = $env:PS4_PAYLOAD_SDK }
  if (-not $SdkPath) { throw 'Pass -SdkPath with the Linux path to ps4-payload-sdk, or set PS4_PAYLOAD_SDK.' }
  $windowsScript = (Join-Path $PSScriptRoot 'build.sh').Replace('\', '/')
  $scriptPath = & wsl -d $WslDistribution -- wslpath -a $windowsScript
  if ($LASTEXITCODE -ne 0) { throw 'Cannot translate build path for WSL.' }
  $scriptPath = $scriptPath.Trim()
  $imagePaste = if ($Ps4ImagePaste) { 1 } else { 0 }
  $nativeReadWait = if ($Ps4NativeReadWait) { 1 } else { 0 }
  $observers = if ($Ps4Observers) { 1 } else { 0 }
  $lifecycle = if ($Ps4RemoteLifecycle) { 1 } else { 0 }
  $idleRun = if ($Ps4RemoteIdleRun) { 1 } else { 0 }
  $admissionProbe = if ($Ps4AdmissionProbe) { 1 } else { 0 }
  $latchBypass = if ($Ps4LatchBypass) { 1 } else { 0 }
  $budgetUnlock = if ($Ps4BudgetUnlock) { 1 } else { 0 }
  $daemonDump = if ($Ps4DaemonDump) { 1 } else { 0 }
  $encoderState = if ($Ps4EncoderState) { 1 } else { 0 }
  $lowMemory = if ($Ps4LowMemory -or $Ps4Remote1080) { 1 } else { 0 }
  $nativeSource = if ($Ps4NativeSource) { 1 } else { 0 }
  $vencJournal = if ($Ps4VencJournal) { 1 } else { 0 }
  $remote1080 = if ($Ps4Remote1080) { 1 } else { 0 }
  $buildArgs = @('wsl', '-d', $WslDistribution, '--', 'env',
    "PS4_PAYLOAD_SDK=$SdkPath", "PBS_BUILD_CONFIG=$Config",
    ("PBS_PS4_CAPTURE_HEIGHT=" + $Ps4Resolution.TrimEnd('p')),
    "PBS_PS4_CAPTURE_FPS=$Ps4FrameRate",
    "PBS_PS4_ADMISSION_PROBE=$admissionProbe",
    "PBS_PS4_LATCH_BYPASS=$latchBypass",
    "PBS_PS4_BUDGET_UNLOCK=$budgetUnlock",
    "PBS_PS4_DAEMON_DUMP=$daemonDump",
    "PBS_PS4_ENCODER_STATE=$encoderState",
    "PBS_PS4_LOW_MEMORY=$lowMemory",
    "PBS_PS4_REMOTE_1080=$remote1080",
    "PBS_PS4_VENC_JOURNAL=$vencJournal",
    "PBS_PS4_NATIVE_SOURCE=$nativeSource",
    "PBS_PS4_MEMORY_COUNT=$Ps4MemoryCount",
    "PBS_PS4_CAPTURE_USAGE=$Ps4Usage", "PBS_PS4_IMAGE_PASTE=$imagePaste",
    "PBS_PS4_READ_WAIT_EVENTS=$nativeReadWait", "PBS_PS4_SMOKE_OBSERVERS=$observers",
    "PBS_PS4_REMOTE_LIFECYCLE=$lifecycle", "PBS_PS4_REMOTE_IDLE_RUN=$idleRun",
    "PBS_PS4_WORKER_BOOT=$Ps4WorkerBoot",
    "PBS_PS4_CAPTURE_SECONDS=$Ps4CaptureSeconds",
    ("PBS_PS4_WORKER_PROBE=" + $(if ($env:PBS_PS4_WORKER_PROBE) { $env:PBS_PS4_WORKER_PROBE } else { "0" })))
  if ($Ps4Avcap -eq 'remote') { $buildArgs += 'PBS_PS4_REMOTE_AVCAP=1' }
  elseif ($Ps4Avcap -eq 'local') { $buildArgs += 'PBS_PS4_REMOTE_AVCAP=0' }
  $buildArgs += @('bash', $scriptPath, 'ps4', $Kind)
  $wslArgs = $buildArgs[1..($buildArgs.Count - 1)]
  & $buildArgs[0] @wslArgs
  if ($LASTEXITCODE -ne 0) { throw "PS4 build failed (exit $LASTEXITCODE)" }
  return
}
if ($Kind -ne 'stream') { throw 'The probe build is available for PS4 only.' }
if (-not $SdkPath) { $SdkPath = $env:PS5_PAYLOAD_SDK }

$generated = Join-Path $PSScriptRoot 'build/generated'
New-Item -ItemType Directory -Force -Path $generated | Out-Null

$viewer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\h264.html') -Raw
$muxer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\h264-mux.js') -Raw
$viewer = $viewer.Replace('/*PBS_MUX*/', $muxer)
$escapedViewer = $viewer.Replace('\', '\\').Replace('"', '\"').Replace("`r", '').Replace("`n", '\n')
$viewerHeader = '#pragma once' + "`n" + 'static const char PBS_H264_HTML[] = "' + $escapedViewer + '";' + "`n"
[System.IO.File]::WriteAllText((Join-Path $generated 'webui.h'), $viewerHeader, [System.Text.UTF8Encoding]::new($false))
$bannerBytes = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'web\banner.png'))
$bannerHeader = '#pragma once' + "`n" + 'static const unsigned char PBS_BANNER_BYTES[] = {' + ($bannerBytes -join ',') + '};' + "`n"
[System.IO.File]::WriteAllText((Join-Path $generated 'banner.h'), $bannerHeader, [System.Text.UTF8Encoding]::new($false))

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

$sources = @('platform/ps5/main.c', 'common/process/inject.c', 'common/http/server.c') | ForEach-Object { Join-Path $PSScriptRoot ('src/' + $_) }

$kernelHeader = Join-Path $sdk 'target\include\ps5\kernel.h'
$sdkDefines = @()
if($TesterRelease){$sdkDefines += '-DPBS_TESTER_RELEASE'}
if($Ps5NoNativeRateDefault){$sdkDefines += '-DPBS_PS5_NO_NATIVE_RATE_DEFAULT'}
if ((Test-Path $kernelHeader) -and
    ((Get-Content -LiteralPath $kernelHeader -Raw) -match 'kernel_get_ucred_attrs\s*\(\s*pid_t\s+pid\s*,\s*uint8_t')) {
  $sdkDefines += '-DPBS_UCRED_ATTRS_ARRAY'
}

$includeFlags = @('-I', (Join-Path $PSScriptRoot 'src'), '-I', $generated)
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
  ) + $includeFlags + $cflags + $sdkDefines + @(
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
  ) + $includeFlags + $cflags + $sdkDefines + @(
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
