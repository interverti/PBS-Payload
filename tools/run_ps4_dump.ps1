# Read-only PS4 offset dump: build (optional), deploy, fetch dump.txt/offsets.txt.
# The payload only measures; it never patches, attaches or opens a capture session.
[CmdletBinding()]
param(
  [switch]$Rebuild,
  [string]$SdkPath = $env:PS4_PAYLOAD_SDK,
  [string]$WslDistribution = 'Debian',
  [string]$ConsoleAddress = '192.168.1.25',
  [ValidateRange(1,65535)][int]$LoaderPort = 9090,
  [ValidateRange(1,65535)][int]$FtpPort = 2121,
  [string]$ArtifactsDir,
  [ValidateRange(0,120)][int]$WaitSeconds = 10,
  [switch]$PreflightOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$payload = Join-Path $root 'payload/build/ps4/pbs-dump.elf'

if (-not $ArtifactsDir) {
  $run = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + ([guid]::NewGuid().ToString('N').Substring(0,8))
  $ArtifactsDir = Join-Path $root ".local-research/ps4/dump/$run"
}
$ignored = [IO.Path]::GetFullPath((Join-Path $root '.local-research'))
$target = [IO.Path]::GetFullPath($ArtifactsDir)
if (-not $target.StartsWith($ignored + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
  throw 'ArtifactsDir must be inside the ignored .local-research directory.'
}

if ($Rebuild -or -not (Test-Path -LiteralPath $payload)) {
  if (-not $SdkPath) { throw 'Set PS4_PAYLOAD_SDK (WSL path) or pass -SdkPath to build.' }
  & (Join-Path $root 'payload/build.ps1') -Platform PS4 -Kind dump -WslDistribution $WslDistribution -SdkPath $SdkPath
  if ($LASTEXITCODE) { throw 'Dump build failed.' }
}
if (-not (Test-Path -LiteralPath $payload)) {
  throw "Missing dump payload: $payload. Build it with: ./payload/build.ps1 -Platform PS4 -Kind dump"
}
$curl = Get-Command curl.exe -ErrorAction SilentlyContinue
if (-not $curl) { throw 'curl.exe is required to fetch the report over FTP.' }
New-Item -ItemType Directory -Force -Path $target | Out-Null
if ($PreflightOnly) {
  Write-Host "Preflight OK: payload, curl and artifact directory are ready. Nothing was uploaded or fetched."
  return
}

# The ELF loader is one-shot: enable it on the console right before this call.
& (Join-Path $PSScriptRoot 'deploy.ps1') -Payload $payload -ConsoleAddress $ConsoleAddress -Port $LoaderPort
if ($LASTEXITCODE) { throw 'ELF upload failed.' }
Write-Host "Waiting $WaitSeconds s for the dump payload to finish..."
Start-Sleep -Seconds $WaitSeconds

$ftpBase = "ftp://${ConsoleAddress}:${FtpPort}/data/pbs/ps4"
$fetched = 0
foreach ($name in @('dump.txt', 'offsets.txt')) {
  $destination = Join-Path $target $name
  & $curl.Source --fail --silent --show-error --connect-timeout 5 --max-time 20 `
    --user anonymous:anonymous --output $destination "$ftpBase/$name"
  if ($LASTEXITCODE -eq 0) {
    $fetched++
    Write-Host "Fetched $destination"
  } else {
    Write-Warning "Cannot fetch $name yet; the payload may still be running or the FTP service is unavailable."
  }
}
if (-not $fetched) {
  throw "No report fetched from $ftpBase. Check the console FTP service and that the payload ran."
}

$report = Join-Path $target 'dump.txt'
if (Test-Path -LiteralPath $report) {
  Write-Host ''
  Get-Content -LiteralPath $report | Select-Object -First 40 | Write-Host
  Write-Host ''
  Write-Host "Full report: $report"
}
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python -and (Test-Path -LiteralPath (Join-Path $target 'offsets.txt'))) {
  & $python.Source (Join-Path $root 'tools/ps4_offsets.py') (Join-Path $target 'offsets.txt') `
    --json (Join-Path $target 'offsets.json') --snippet (Join-Path $target 'profile.h')
  # Close the loop: the fresh report must agree with the recorded table.
  $report = Join-Path $target 'dump.txt'
  $header = Select-String -LiteralPath (Join-Path $target 'offsets.txt') `
    -Pattern 'firmware=(0x[0-9a-fA-F]+)' | Select-Object -First 1
  if ($header -and (Test-Path -LiteralPath $report)) {
    $version = $header.Matches[0].Groups[1].Value
    Write-Host ''
    Write-Host "Checking the report against tools/data/ps4_firmware_offsets.json ($version):"
    & $python.Source (Join-Path $root 'tools/ps4_firmware_table.py') --report $report `
      --firmware $version
    if ($LASTEXITCODE) {
      Write-Warning 'The report differs from the recorded table; update tools/data/ps4_firmware_offsets.json after review.'
    }
  }
  Write-Host ''
  Write-Host 'Fetch the module ELFs with the probe payload, then:'
  Write-Host "  python ./tools/ps4_offsets.py '$target/offsets.txt' --module .local-research/ps4/libSceAvcap.elf"
}
