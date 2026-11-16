<#
.SYNOPSIS
  Compile (and optionally upload) a KVM sketch with arduino-cli.

.EXAMPLE
  .\scripts\build.ps1 firmware\bringup\01_chipinfo
  .\scripts\build.ps1 firmware\bringup\01_chipinfo -Port COM7        # compile + upload
  .\scripts\build.ps1 firmware\pico_a -Freq 240 -Define KVM_LINK_BAUD=3000000
  .\scripts\build.ps1 -ListPorts

.NOTES
  Uses arduino-cli from PATH, else the copy bundled with Arduino IDE 2.
  First upload of a blank Pico: hold BOOTSEL while plugging in; it mounts as a
  drive and arduino-cli uploads via the UF2 path. After that, -Port works.
#>
param(
  [Parameter(Position = 0)] [string] $Sketch,
  [string]   $Port,
  [int]      $Freq = 120,               # PIO-USB needs a multiple of 12 MHz (120 / 240)
  [string]   $Opt  = 'Optimize2',       # -O2
  [string[]] $Define = @(),             # extra -D flags, e.g. KVM_LINK_BAUD=3000000
  [switch]   $ListPorts
)
$ErrorActionPreference = 'Stop'

$IndexUrl = 'https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json'
$Repo     = Split-Path -Parent $PSScriptRoot
$Libs     = Join-Path $Repo 'firmware\libraries'

$Cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $Cli) {
  $Cli = Join-Path $env:ProgramFiles 'Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
  if (-not (Test-Path $Cli)) { throw 'arduino-cli not found (PATH or Arduino IDE 2 bundle).' }
}

if ($ListPorts) { & $Cli board list --additional-urls $IndexUrl; exit $LASTEXITCODE }
if (-not $Sketch) { throw 'Pass a sketch folder, e.g. firmware\bringup\01_chipinfo' }

# Pico 2, Adafruit TinyUSB stack, 64 KB flash region reserved for the press-log ring.
$Fqbn = "rp2040:rp2040:rpipico2:usbstack=tinyusb,freq=$Freq,opt=$Opt,flash=4194304_65536"

$CliArgs = @('compile', '--fqbn', $Fqbn, '--additional-urls', $IndexUrl, '--libraries', $Libs, '--warnings', 'default')
if ($Define.Count -gt 0) {
  $flags = ($Define | ForEach-Object { "-D$_" }) -join ' '
  $CliArgs += @('--build-property', "compiler.cpp.extra_flags=$flags", '--build-property', "compiler.c.extra_flags=$flags")
}
if ($Port) { $CliArgs += @('--upload', '--port', $Port) }
$CliArgs += (Resolve-Path $Sketch).Path

Write-Host "arduino-cli $($CliArgs -join ' ')" -ForegroundColor DarkGray
# Compiler diagnostics go to stderr; with ErrorActionPreference=Stop, PowerShell 5.1
# would turn the first stderr line into a terminating error and hide the rest.
$ErrorActionPreference = 'Continue'
& $Cli @CliArgs 2>&1 | ForEach-Object { "$_" }
exit $LASTEXITCODE
