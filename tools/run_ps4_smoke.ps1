# SPDX-License-Identifier: GPL-3.0-or-later
# Single-shot PS4 end-to-end: optional rebuild, ONE ELF loader connection,
# FTP evidence with remote timestamps, and offline decoded-frame validation.
[CmdletBinding()]
param(
  [string]$ConsoleAddress = '192.168.1.25',
  [ValidateRange(1,65535)][int]$LoaderPort = 9090,
  [ValidateRange(1,65535)][int]$FtpPort = 2121,
  [ValidateRange(11,60)][int]$WaitSeconds = 12,
  [ValidateRange(1,3600)][int]$CaptureSeconds = 10,
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
  [switch]$Ps4RemoteLifecycle,
  [switch]$Ps4RemoteIdleRun,
  [string]$SdkPath = $env:PS4_PAYLOAD_SDK,
  [string]$WslDistribution = 'Debian',
  [switch]$Rebuild,
  [switch]$PreflightOnly,
  [string]$ArtifactsDir
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ($PreflightOnly -and $Rebuild) {
  throw '-PreflightOnly cannot be combined with -Rebuild (preflight must not modify files).'
}
if ($Ps4Remote1080) {
  if ($Ps4Resolution -ne '1080p' -or $Ps4FrameRate -ne 30 -or $Ps4Usage -ne 0 -or $Ps4ImagePaste -or $Ps4LatchBypass -or $Ps4BudgetUnlock -or $Ps4AdmissionProbe -or $Ps4RemoteLifecycle -or $Ps4RemoteIdleRun -or $CaptureSeconds -gt 10) {
    throw '-Ps4Remote1080 requires a normal 1080p30 smoke, at most 10 seconds, without other experimental flags.'
  }
  $Ps4LowMemory = [switch]::new($true)
}
if ($Ps4EncoderState) {
  if ($Ps4Resolution -ne '720p' -or $Ps4FrameRate -ne 60 -or $Ps4Usage -ne 0 -or $Ps4ImagePaste -or $Ps4LatchBypass -or $Ps4BudgetUnlock -or $Ps4LowMemory -or $Ps4Remote1080 -or $Ps4AdmissionProbe -or $Ps4VencJournal -or $Ps4NativeSource -or $Ps4DaemonDump -or $Ps4RemoteLifecycle -or $Ps4RemoteIdleRun -or $CaptureSeconds -gt 10) {
    throw '-Ps4EncoderState requires the standard 720p60 smoke alone, at most 10 seconds.'
  }
}
$filename = 'pbs-smoke'
if ($CaptureSeconds -ne 10) { $filename += "-${CaptureSeconds}s" }
if ($Ps4Resolution -eq '1080p') { $filename += '-1080p' }
if ($Ps4FrameRate -eq 30) {
  if ($Ps4Resolution -ne '1080p') { throw '30fps experiment requires 1080p.' }
  $filename += '-30fps'
}
if ($Ps4Usage -eq 1) { $filename += '-usage1' }
if ($Ps4ImagePaste) { $filename += '-imagepaste' }
if ($Ps4RemoteLifecycle) {
  if ($Ps4Usage -ne 0 -or $Ps4ImagePaste) {
    throw '-Ps4RemoteLifecycle is only supported by the normal usage=0 smoke build.'
  }
  $filename += '-lifecycle'
}
if ($Ps4RemoteIdleRun) { $filename += '-idle-run' }
if ($Ps4AdmissionProbe -or $Ps4LatchBypass) {
  if ($Ps4Resolution -ne '1080p' -or $Ps4FrameRate -ne 30 -or $Ps4Usage -ne 0 -or $Ps4ImagePaste -or $Ps4RemoteLifecycle -or $Ps4RemoteIdleRun) {
    throw '-Ps4AdmissionProbe requires normal remote 1080p30 smoke.'
  }
  $filename += '-admission'
}
if ($Ps4LatchBypass) { $filename += '-latch' }
if ($Ps4BudgetUnlock) { $filename += '-budget' }
if ($Ps4DaemonDump) { $filename += '-daemondump' }
if ($Ps4LowMemory) {
  if ($Ps4Resolution -ne '1080p' -or $Ps4FrameRate -ne 30 -or (-not $Ps4LatchBypass -and -not $Ps4Remote1080) -or $Ps4BudgetUnlock -or $CaptureSeconds -gt 10) {
    throw '-Ps4LowMemory requires 1080p30 latch or Remote1080 smoke, at most 10 seconds, without the Neo budget flag.'
  }
  $filename += '-lowmem'
}
if ($Ps4Remote1080) { $filename += '-rp24' }
if ($Ps4MemoryCount -ne 3) {
  if (-not $Ps4LowMemory) { throw '-Ps4MemoryCount requires a bounded low-memory smoke.' }
  $filename += "-memory$Ps4MemoryCount"
}
if ($Ps4VencJournal) {
  if (-not $Ps4Remote1080) { throw '-Ps4VencJournal requires -Ps4Remote1080.' }
  $filename += '-vencjournal'
}
if ($Ps4NativeSource) {
  if (-not $Ps4Remote1080 -or $Ps4VencJournal) { throw '-Ps4NativeSource requires Remote1080 without journal.' }
  $filename += '-nativesource'
}
if ($Ps4EncoderState) { $filename += '-encoderstate' }
$filename += '.elf' 
$payload = Join-Path $root "payload/build/ps4/$filename"
if (-not $ArtifactsDir) {
  # Never overwrite evidence from a prior one-shot loader capture.
  $run = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + ([guid]::NewGuid().ToString('N').Substring(0,8))
  $ArtifactsDir = Join-Path $root ".local-research/ps4/e2e/$run"
}
if ($Rebuild) {
  if (-not $SdkPath) { throw 'Set PS4_PAYLOAD_SDK (WSL path) or pass -SdkPath to rebuild.' }
  & (Join-Path $root 'payload/build.ps1') -Platform PS4 -Kind smoke -Ps4FrameRate $Ps4FrameRate -Ps4Resolution $Ps4Resolution -Ps4AdmissionProbe:$Ps4AdmissionProbe -Ps4LatchBypass:$Ps4LatchBypass -Ps4BudgetUnlock:$Ps4BudgetUnlock -Ps4DaemonDump:$Ps4DaemonDump -Ps4EncoderState:$Ps4EncoderState -Ps4LowMemory:$Ps4LowMemory -Ps4Remote1080:$Ps4Remote1080 -Ps4VencJournal:$Ps4VencJournal -Ps4NativeSource:$Ps4NativeSource -Ps4MemoryCount $Ps4MemoryCount -Ps4CaptureSeconds $CaptureSeconds -Ps4Usage $Ps4Usage -Ps4ImagePaste:$Ps4ImagePaste -Ps4Avcap remote -Ps4RemoteLifecycle:$Ps4RemoteLifecycle -Ps4RemoteIdleRun:$Ps4RemoteIdleRun -WslDistribution $WslDistribution -SdkPath $SdkPath
  if ($LASTEXITCODE) { throw 'Smoke build failed.' }
}
if (-not (Test-Path -LiteralPath $payload)) { throw "Build the PS4 smoke payload first: $payload" }
$ignored = [IO.Path]::GetFullPath((Join-Path $root '.local-research'))
$target = [IO.Path]::GetFullPath($ArtifactsDir)
if (-not $target.StartsWith($ignored + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
  throw 'ArtifactsDir must be inside the ignored .local-research directory.'
}
# Preflight EVERYTHING required for offline verification before consuming the
# one-shot ELF loader. -PreflightOnly performs no builds, FTP access or upload.
$validator = Join-Path $PSScriptRoot 'verify_ps4_capture.py'
if (-not (Test-Path -LiteralPath $validator)) { throw "Video validator missing: $validator" }
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $python) { throw 'Python 3.11+ is required for tools/verify_ps4_capture.py.' }
& $python.Source -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)'
if ($LASTEXITCODE -ne 0) { throw 'The selected Python must be version 3.11 or later.' }
foreach ($commandName in @('curl.exe', 'ffmpeg.exe', 'ffprobe.exe')) {
  $command = Get-Command $commandName -ErrorAction SilentlyContinue
  if (-not $command) { throw "Required tool $commandName is not available on PATH." }
  $versionFlag = if ($commandName -eq 'curl.exe') { '--version' } else { '-version' }
  & $command.Source $versionFlag *> $null
  if ($LASTEXITCODE -ne 0) { throw "Required tool $commandName could not execute successfully." }
}
if ($PreflightOnly) {
  Write-Host "PS4 smoke preflight passed: $filename, Python, curl, FFmpeg and FFprobe available; NO loader/FTP connection made."
  return
}
# A failed capture contains useful evidence and must not be replaced silently.
foreach ($name in @('capture.txt', 'problems.txt', 'smoke.h264', 'http.txt', 'verification.json', 'first-frame.png')) {
  if (Test-Path -LiteralPath (Join-Path $ArtifactsDir $name)) {
    throw "Refusing to overwrite existing $name in $ArtifactsDir. Choose a new -ArtifactsDir."
  }
}
New-Item -ItemType Directory -Force -Path $ArtifactsDir | Out-Null
$artifacts = (Resolve-Path -LiteralPath $ArtifactsDir).Path
$beforeDeploy = (Get-Date).ToUniversalTime()
[ordered]@{
  payload = $payload
  sha256 = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash
  bytes = (Get-Item -LiteralPath $payload).Length
  capture_seconds = $CaptureSeconds
  resolution = $Ps4Resolution
  requested_fps = $Ps4FrameRate
  admission_snapshot = [bool]($Ps4AdmissionProbe -or $Ps4LatchBypass -or $Ps4BudgetUnlock -or $Ps4Remote1080)
  latch_patch = [bool]$Ps4LatchBypass
  budget_unlock = [bool]$Ps4BudgetUnlock
  daemon_dump = [bool]$Ps4DaemonDump
  encoder_state = [bool]$Ps4EncoderState
  low_memory = [bool]$Ps4LowMemory
  remote_1080 = [bool]$Ps4Remote1080
  venc_journal = [bool]$Ps4VencJournal
  native_source = [bool]$Ps4NativeSource
  memory_count = $Ps4MemoryCount
  sent_at_utc = $beforeDeploy.ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $artifacts 'deployment.json')
Write-Host 'Sending the smoke ELF to the one-shot loader (one connection only).'
& (Join-Path $PSScriptRoot 'deploy.ps1') -Payload $payload -ConsoleAddress $ConsoleAddress -Port $LoaderPort
if (-not $?) { throw 'ELF transfer failed. Re-enable the loader on the console before trying again.' }
Write-Host "Waiting at least $WaitSeconds seconds for the capture report and diagnostic video."
Start-Sleep -Seconds $WaitSeconds

$ftpBase = "ftp://${ConsoleAddress}:${FtpPort}/data/pbs/ps4"
$captureLog = Join-Path $artifacts 'capture.txt'
$complete = $false
# The startup handshake may add several seconds to the ten-second capture.
# Poll only the read-only FTP report, never the one-shot ELF loader again.
$reportDeadline = $beforeDeploy.AddSeconds($CaptureSeconds + 90)
while ((Get-Date).ToUniversalTime() -lt $reportDeadline) {
  & curl.exe --fail --silent --show-error --connect-timeout 5 --max-time 10 --remote-time --user anonymous:anonymous --output $captureLog "$ftpBase/capture.txt"
  # A fatal payload exception cannot append exit=N. Even when curl reports a
  # truncated FTP data connection, its locally preserved capture.txt can
  # already contain the crash. Stop the polling immediately rather than
  # waiting for an exit marker that will never arrive. Remote timestamps
  # prevent treating a previous smoke's crash report as today's outcome.
  if (Test-Path -LiteralPath $captureLog) {
    $logTime = (Get-Item -LiteralPath $captureLog).LastWriteTimeUtc
    if ($logTime -ge $beforeDeploy.AddMinutes(-2)) {
      $partialLog = Get-Content -LiteralPath $captureLog -Raw
      # Accept both the corrected rip= and original rip reports: the
      # previous smoke's signal formatter overwrote each '=' and printed one
      # extra trailing zero. Preserve the raw line rather than interpreting
      # a potentially shifted instruction pointer as an exact address.
      if ($partialLog -match '(?m)^CRASH sig=[0-9a-f]{2} rip=?[0-9a-f]{16,17} address=?[0-9a-f]{16,17}\s*$') {
        throw "PS4 payload crashed before normal cleanup: $($Matches[0]); preserved $captureLog. DO NOT redeploy before reviewing the crash."
      }
    }
  }
  if ($LASTEXITCODE -eq 0) {
    $logTime = (Get-Item -LiteralPath $captureLog).LastWriteTimeUtc
    if ($logTime -ge $beforeDeploy.AddMinutes(-2) -and
        (Get-Content -LiteralPath $captureLog -Raw) -match '(?m)^exit=[0-9]+\s*$') {
      $complete = $true
      break
    }
  }
  Start-Sleep -Seconds 2
}
if (-not $complete) { throw "No fresh completed capture report on FTP. Inspect $captureLog and the console before another ELF attempt." }
$completedLog = Get-Content -LiteralPath $captureLog -Raw
if ($completedLog -notmatch "(?m)^PBS PS4 fw=.* seconds=$CaptureSeconds usage=") {
  throw 'Runtime duration does not match the selected ELF. Preserve evidence and inspect the build.'
}
# The compact problem index records every detected problem the moment it
# happens and ends with the first problem as root cause. Fetch it before any
# failure decision can stop this runner.
$problemsLog = Join-Path $artifacts 'problems.txt'
& curl.exe --fail --silent --show-error --connect-timeout 5 --max-time 10 --remote-time --user anonymous:anonymous --output $problemsLog "$ftpBase/problems.txt"
if ($LASTEXITCODE -ne 0) {
  if (Test-Path -LiteralPath $problemsLog) { Remove-Item -LiteralPath $problemsLog -Force }
  Write-Warning 'Compact problems.txt index unavailable over FTP; diagnosis falls back to capture.txt only.'
}
# Instant root-cause diagnosis from the preserved evidence, printed for every
# run so a failed smoke is triaged without opening the full report by hand.
$diagnoser = Join-Path $PSScriptRoot 'diagnose_ps4_log.py'
$diagnosisText = ''
if (Test-Path -LiteralPath $diagnoser) {
  $diagnosisCommand = @($captureLog)
  if (Test-Path -LiteralPath $problemsLog) { $diagnosisCommand += @('--problems', $problemsLog) }
  # Capture stdout only: stderr must reach the console untouched and must not
  # be turned into a terminating error record by ErrorActionPreference=Stop.
  $diagnosisText = (& $python.Source $diagnoser $diagnosisCommand | Out-String).Trim()
  if ($LASTEXITCODE -gt 1) {
    Write-Warning "Diagnosis is inconclusive (exit $LASTEXITCODE); inspect $captureLog manually."
  }
  if ($diagnosisText) {
    Write-Host 'PS4 problem diagnosis:'
    Write-Host $diagnosisText
  }
} else {
  Write-Warning "Diagnoser missing ($diagnoser); falling back to report regexes."
}
# Retrieve the diagnostic before an expected Open failure can stop this runner.
if ($Ps4DaemonDump) {
  $dumpFile = Join-Path $artifacts 'daemon-dump.bin'
  & curl.exe --fail --silent --show-error --connect-timeout 5 --max-time 60 --remote-time --user anonymous:anonymous --output $dumpFile "$ftpBase/daemon-dump.bin"
  if ($LASTEXITCODE -eq 0) {
    $dumpInfo = Get-Item -LiteralPath $dumpFile
    Write-Host ("Daemon image dump retrieved: {0} bytes" -f $dumpInfo.Length)
  } else {
    Write-Warning 'daemon-dump.bin unavailable over FTP; check the Daemon dump line in capture.txt.'
  }
}
if ($Ps4LowMemory -or $Ps4EncoderState) {
  foreach ($snapshotName in @('encoder-before-start.bin', 'encoder-after-start.bin', 'encoder-inflight.bin', 'encoder-state.bin')) {
    $snapshotPattern = '(?m)^Encoder snapshot: rc=0 path=/data/pbs/ps4/' + [regex]::Escape($snapshotName) + ' '
    if ($completedLog -match $snapshotPattern) {
      & curl.exe --fail --silent --show-error --connect-timeout 5 --max-time 10 --remote-time --user anonymous:anonymous --output (Join-Path $artifacts $snapshotName) "$ftpBase/$snapshotName"
      if ($LASTEXITCODE -ne 0) { Write-Warning "$snapshotName unavailable over FTP." }
    }
  }
}
if ($Ps4VencJournal -and $completedLog -match '(?m)^Venc journal file: rc=0 ') {
  & curl.exe --fail --silent --show-error --connect-timeout 5 --max-time 10 --remote-time --user anonymous:anonymous --output (Join-Path $artifacts 'venc-journal.bin') "$ftpBase/venc-journal.bin"
  if ($LASTEXITCODE -ne 0) { Write-Warning 'Venc printf journal unavailable over FTP.' }
}
$runtimeFailure = $null
if ($completedLog -match '(?m)^exit=([1-9][0-9]*)\s*$') {
  $failedExit = $Matches[1]
  # Prefer the analyzer's root cause: it names the first real failure (with the
  # decoded code and the operator hint) instead of a downstream symptom.
  $primaryDetail = [regex]::Match($diagnosisText, '(?m)^\s+detail: (.*)$')
  $primaryHint = [regex]::Match($diagnosisText, '(?m)^\s+hint:\s+(.*)$')
  # No smoke.h264 is created if OpenVideoEx fails before Start. Preserve and
  # report the real Avcap/cleanup failure, not a misleading FTP 550/78 error.
  $openFailure = [regex]::Match($completedLog, '(?m)^OpenVideoEx profile=\d+ -> [0-9a-f]{8} handle=.*$')
  $readFailure = [regex]::Match($completedLog, '(?m)^Remote ReadVideo failed rc=[0-9a-f]{8}.*$')
  $cause = if ($primaryDetail.Success) {
    $primaryDetail.Groups[1].Value + $(if ($primaryHint.Success) { ' -> ' + $primaryHint.Groups[1].Value } else { '' })
  } elseif ($readFailure.Success) { $readFailure.Value } elseif ($openFailure.Success) { $openFailure.Value } else { 'See capture.txt' }
  $slots = [regex]::Match($completedLog, '(?m)^Avcap encoder slots at final OpenVideoEx 80a70008: mask=([0-9a-f]{8}) slot0=([01]) \(read-only\)\s*$')
  if ($slots.Success -and -not $primaryHint.Success) {
    $cause += "; encoder slot mask=$($slots.Groups[1].Value), slot0=$($slots.Groups[2].Value) (read-only)"
  }
  $runtimeFailure = "PS4 smoke completed with exit=$failedExit; $cause; evidence preserved at $captureLog. No second ELF attempt."
  if ($completedLog -notmatch '(?m)^Smoke Annex B recorded [1-9]\d* bytes\s*$') {
    throw $runtimeFailure
  }
  # A late read/cleanup error can follow useful hardware video. Retrieve and
  # decode that recording before reporting failure; never turn it into PASS.
  Write-Warning $runtimeFailure
}
foreach ($file in @('smoke.h264', 'http.txt')) {
  $destination = Join-Path $artifacts $file
  & curl.exe --fail --silent --show-error --connect-timeout 5 --max-time 30 --remote-time --user anonymous:anonymous --output $destination "$ftpBase/$file"
  if ($LASTEXITCODE -ne 0) {
    if ($file -eq 'http.txt') { Write-Warning 'Optional HTTP diagnostic log unavailable.'; continue }
    throw "Failed to fetch $file from console FTP (exit $LASTEXITCODE)."
  }
}
$videoFile = Join-Path $artifacts 'smoke.h264'

if ((Get-Item -LiteralPath $videoFile).LastWriteTimeUtc -lt $beforeDeploy.AddMinutes(-2)) {
  throw 'The remote smoke.h264 is older than this capture. Refusing to validate a stale recording.'
}
Write-Host 'PS4 runtime report:'
Get-Content -LiteralPath $captureLog | Select-Object -Last 32
$reportText = Get-Content -LiteralPath $captureLog -Raw
# Decode even when teardown later proves unsafe: a one-shot experiment must
# preserve its pixel verdict and independently report the cleanup failure.
$maxFrames = [Math]::Max(3600, $CaptureSeconds * 61 + 120)
& $python.Source $validator $videoFile --assumed-fps $Ps4FrameRate --max-frames $maxFrames --report (Join-Path $artifacts 'verification.json')
$verificationExit = $LASTEXITCODE
if (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue) {
  & ffmpeg.exe -hide_banner -loglevel error -y -i $videoFile -frames:v 1 (Join-Path $artifacts 'first-frame.png')
}
if ($reportText -match 'PS4 debug gate restore pending|RemotePlay Term uncertain|Refusing new capture: previous RemotePlay Term|Avcap release quarantined|capture request ownership is uncertain|RemotePlay Init timeout|SetStaticInfo outcome uncertain|local Avcap Term failed|Avcap cleanup incomplete|OpenVideoEx failure quarantined|RemotePlay process replaced|process birth unavailable') {
  throw 'PS4 cleanup/ownership is uncertain. Inspect capture.txt; do not run a second payload until the console state is verified.'
}
if ($reportText -match 'transport_packets=\d+ video_pixels=UNVERIFIED_NEEDS_FFMPEG') {
  Write-Host 'Encoded video transport confirmed; actual pixels require independent FFmpeg verification.'
}
if ($reportText -notmatch 'SceAvSetting output snapshot after OpenVideoEx:' -or
    $reportText -notmatch 'ShellCore RemotePlay status after OpenVideoEx:') {
  Write-Warning 'One or more non-invasive VideoOut/RemotePlay status observers are unavailable. Decoded frame validation remains authoritative.'
}
$nativeBefore = [regex]::Match($reportText, '(?m)^SceAvSetting output snapshot before init: rc=00000000 base_mode=(\d+) raw40=([0-9a-f]{80})\r?$')
$nativeOpened = [regex]::Match($reportText, '(?m)^SceAvSetting output snapshot after OpenVideoEx: rc=00000000 base_mode=(\d+) raw40=([0-9a-f]{80})\r?$')
if ($nativeBefore.Success -and $nativeOpened.Success) {
  Write-Host "SceAvSetting base_mode: before=$($nativeBefore.Groups[1].Value) after_open=$($nativeOpened.Groups[1].Value)"
  if ($nativeBefore.Groups[2].Value -eq $nativeOpened.Groups[2].Value) {
    Write-Warning 'Read-only output descriptor was unchanged across OpenVideoEx. This does not prove that hardware scan-in is active or inactive.'
  }
}
$busBefore = [regex]::Match($reportText, '(?m)^SceVideoOut bus snapshot before init: rc=00000000 enum_byte6=([0-9a-f]{2}) flags=([0-9a-f]{8}) native_bit1=([01]) raw40=([0-9a-f]{80})\r?$')
$busStarted = [regex]::Match($reportText, '(?m)^SceVideoOut bus snapshot after Start: rc=00000000 enum_byte6=([0-9a-f]{2}) flags=([0-9a-f]{8}) native_bit1=([01]) raw40=([0-9a-f]{80})\r?$')
if ($busBefore.Success -and $busStarted.Success) {
  Write-Host "VideoOut bus before (enum=$($busBefore.Groups[1].Value), flags=$($busBefore.Groups[2].Value)), after Start (enum=$($busStarted.Groups[1].Value), flags=$($busStarted.Groups[2].Value))"
  if ($busBefore.Groups[4].Value -eq $busStarted.Groups[4].Value) {
    Write-Warning 'VideoOut bus output descriptor unchanged across Start; this does not prove scan-in is inactive.'
  }
} else {
  Write-Warning 'VideoOut bus read-only status unavailable, or its ABI returned an error; decoded pixels remain authoritative.'
}
foreach ($required in @('(?m)^Pre-Start GetInfo SPS=[1-9]\d* PPS=[1-9]\d* elapsed_ms=\d+\s*$',
                       '(?m)^Stop -> 00000000\s*$',
                       '(?m)^Close -> 00000000\s*$', '(?m)^Term -> 00000000\s*$',
                       $(if ($Ps4FrameRate -eq 60 -or $Ps4Remote1080) { '(?m)^RemotePlay capture request 0 -> 00000000\s*$' } else { '(?m)^Generic capture: RemotePlay-specific request not acquired\s*$' }))) {
  if ($reportText -notmatch $required) {
    throw 'The smoke run did not confirm successful Stop/Close/RemotePlay release/Term. Inspect capture.txt before further deployment.'
  }
}
if ($verificationExit -eq 0) {
  $decoded = Get-Content -Raw -LiteralPath (Join-Path $artifacts 'verification.json') | ConvertFrom-Json
  $expectedHeight = [int]$Ps4Resolution.TrimEnd('p')
  $expectedWidth = if ($expectedHeight -eq 1080) { 1920 } else { 1280 }
  if ($decoded.video.width -ne $expectedWidth -or $decoded.video.height -ne $expectedHeight) {
    throw "Decoded video is $($decoded.video.width)x$($decoded.video.height), expected ${expectedWidth}x${expectedHeight}."
  }
}
if ($verificationExit -ne 0) { throw "Video validation failed (exit $verificationExit). See $artifacts for saved evidence." }
if ($runtimeFailure) { throw $runtimeFailure }
if ($reportText -notmatch '(?m)^exit=0\s*$') {
  throw "Runtime failed despite successful cleanup. Inspect $artifacts/capture.txt; do not infer console stability from the video verifier."
}
Write-Host "PS4 end-to-end video verified; evidence saved at $artifacts"
