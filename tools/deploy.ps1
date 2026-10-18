# Send a complete ELF to the console loader. Do not probe the loader with empty connections.
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Payload,
  [string]$ConsoleAddress = '192.168.1.25',
  [ValidateRange(1,65535)][int]$Port = 9090
)
$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Payload).Path)
if ($bytes.Length -lt 64 -or $bytes[0] -ne 127 -or
    [Text.Encoding]::ASCII.GetString($bytes,1,3) -ne 'ELF') { throw 'Expected an ELF payload.' }
$client = [Net.Sockets.TcpClient]::new()
try {
  try {
    if (!$client.ConnectAsync($ConsoleAddress,$Port).Wait(5000)) { throw 'Connection timed out.' }
  } catch {
    $reason = $_.Exception.GetBaseException().Message
    throw "Cannot connect to the ELF loader at ${ConsoleAddress}:$Port. If this is a one-shot loader, enable it again on the console. Details: $reason"
  }
  $stream = $client.GetStream()
  $stream.WriteTimeout = 10000
  $stream.Write($bytes,0,$bytes.Length)
  $client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
  Write-Host "Sent $($bytes.Length) bytes to ${ConsoleAddress}:$Port. Check the console report for execution status."
  $stream.ReadTimeout = 1000
  $buffer = [byte[]]::new(4096)
  try {
    while (($count = $stream.Read($buffer,0,$buffer.Length)) -gt 0) {
      Write-Host ([Text.Encoding]::UTF8.GetString($buffer,0,$count))
    }
  } catch [IO.IOException] { }
} finally { $client.Dispose() }
