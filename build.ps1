param(
  [string]$Cli = 'arduino-cli',
  [string]$ConfigFile = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$sketchPath = Join-Path $projectRoot 'ESP32_S3'
$buildPath = Join-Path $projectRoot 'build'
$tempPath = Join-Path $projectRoot '.build-tmp'
if (!(Test-Path -LiteralPath (Join-Path $sketchPath 'config_private.h'))) {
  throw 'Missing ESP32_S3/config_private.h. Copy config_private.example.h and fill in your own settings.'
}
if (!(Get-Command $Cli -ErrorAction SilentlyContinue)) {
  throw "arduino-cli not found: $Cli"
}
New-Item -ItemType Directory -Force -Path $buildPath,$tempPath | Out-Null
$env:TEMP = $tempPath
$env:TMP = $tempPath
$compileArgs = @(
  'compile', '--fqbn',
  'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc',
  '--build-path', $buildPath,
  '--build-cache-path', (Join-Path $buildPath 'cache')
)
if ($ConfigFile) { $compileArgs += @('--config-file', $ConfigFile) }
$compileArgs += $sketchPath
& $Cli @compileArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
