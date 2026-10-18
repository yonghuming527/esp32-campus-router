param(
  [Parameter(Mandatory = $true)][string]$Port,
  [string]$Cli = 'arduino-cli',
  [string]$ConfigFile = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildPath = Join-Path $PSScriptRoot 'build'
$firmware = Join-Path $buildPath 'ESP32_S3.ino.bin'
if (!(Get-Command $Cli -ErrorAction SilentlyContinue)) {
  throw "arduino-cli not found: $Cli"
}
if (!(Test-Path -LiteralPath $firmware)) {
  throw 'Missing build/ESP32_S3.ino.bin. Run build.ps1 first.'
}
$uploadArgs = @(
  'upload', '--fqbn',
  'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc',
  '--port', $Port,
  '--input-dir', $buildPath
)
if ($ConfigFile) { $uploadArgs += @('--config-file', $ConfigFile) }
$uploadArgs += (Join-Path $projectRoot 'ESP32_S3')
& $Cli @uploadArgs
if ($LASTEXITCODE -ne 0) { throw "Upload failed ($LASTEXITCODE)" }
