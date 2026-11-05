<#
.SYNOPSIS
  Flashes a firmware image onto an Xteink X4 (ESP32-C3, 16 MB) over USB.

.DESCRIPTION
  Writes the image to app0 (0x10000) and resets otadata (0xE000) so the reader
  boots app0 whichever slot it was running. NVS, the bootloader, the partition
  table and the SD card are not touched. Works for going forward and for going
  back: pass the old image with -Firmware.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\flash-x4.ps1 -DryRun
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\flash-x4.ps1 -Firmware C:\x\borges-x4.bin -Port COM5
#>
[CmdletBinding()]
param(
  [string]$Firmware = (Join-Path $PSScriptRoot '..\.pio\build\default\firmware-x3-x4.bin'),
  [string]$Port = '',
  [switch]$DryRun,
  [switch]$Yes,
  [string]$PioCore = ''
)

$ErrorActionPreference = 'Stop'

$APP0_OFFSET = '0x10000'
$OTADATA_OFFSET = '0xE000'
$APP_SLOT_BYTES = 0x640000      # partitions.csv: app0 and app1
$MIN_IMAGE_BYTES = 64 * 1024
$CHIP_ID_ESP32C3 = 5
$FLASH_16MB_NIBBLE = 4
$BOARD_MAGIC = 'CROSSPOINT-BOARD-V1:'
$BOARD_NAME = 'x4'
$ESPRESSIF_USB = 'VID_303A&PID_1001'   # ESP32-C3 USB Serial/JTAG

function Stop-With([string]$message) {
  Write-Host "REFUSED: $message"
  exit 1
}

function Invoke-Esptool([string[]]$arguments) {
  # esptool writes warnings to stderr. Windows PowerShell 5.1 turns those into
  # terminating errors under 'Stop', so read the exit code instead.
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $output = & $script:Python $script:Esptool @arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previous
  }
  return @{ Code = $code; Output = @($output) }
}

function Find-PioCore {
  $candidates = @()
  if ($PioCore) { $candidates += $PioCore }
  $candidates += (Join-Path $PSScriptRoot '..\..\pio-core')
  $candidates += (Join-Path $env:USERPROFILE '.platformio')
  foreach ($candidate in $candidates) {
    $esptool = Join-Path $candidate 'packages\tool-esptoolpy\esptool.py'
    if ((Test-Path $esptool) -and (Test-Path (Join-Path $candidate 'penv\Scripts\python.exe'))) {
      return (Resolve-Path $candidate).Path
    }
  }
  Stop-With 'esptool not found. Pass the PlatformIO core folder with -PioCore.'
}

function Find-UsbPorts {
  $devices = @(Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -like "USB\$ESPRESSIF_USB*" })
  $ports = @()
  foreach ($device in $devices) {
    if ($device.Name -match '\((COM\d+)\)') { $ports += $Matches[1] }
  }
  return ,$ports
}

# --- Tools ---------------------------------------------------------------------
$core = Find-PioCore
$script:Python = Join-Path $core 'penv\Scripts\python.exe'
$script:Esptool = Join-Path $core 'packages\tool-esptoolpy\esptool.py'
$bootApp0 = Join-Path $core 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
if (-not (Test-Path $bootApp0)) { Stop-With "boot_app0.bin not found: $bootApp0" }

# --- Image checks: none of these need a device ---------------------------------
if (-not (Test-Path $Firmware)) { Stop-With "image not found: $Firmware" }
$Firmware = (Resolve-Path $Firmware).Path
$bytes = [System.IO.File]::ReadAllBytes($Firmware)

if ($bytes.Length -lt $MIN_IMAGE_BYTES) { Stop-With "image too small: $($bytes.Length) bytes." }
if ($bytes.Length -gt $APP_SLOT_BYTES) {
  Stop-With "image is $($bytes.Length) bytes and the app slot holds $APP_SLOT_BYTES."
}
if ($bytes[0] -ne 0xE9) { Stop-With 'this is not an ESP image (bad magic byte).' }

$chipId = [BitConverter]::ToUInt16($bytes, 12)
if ($chipId -ne $CHIP_ID_ESP32C3) {
  Stop-With "image is for chip id $chipId; the X4 is an ESP32-C3 (chip id $CHIP_ID_ESP32C3)."
}
$flashNibble = ($bytes[3] -shr 4)
if ($flashNibble -ne $FLASH_16MB_NIBBLE) {
  Stop-With "image header does not declare 16 MB of flash (nibble $flashNibble)."
}

$text = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
$tagIndex = $text.IndexOf($BOARD_MAGIC, [StringComparison]::Ordinal)
if ($tagIndex -ge 0) {
  $tagEnd = $text.IndexOf(';', $tagIndex)
  $tag = $text.Substring($tagIndex + $BOARD_MAGIC.Length, $tagEnd - $tagIndex - $BOARD_MAGIC.Length)
  if ($tag -ne $BOARD_NAME) { Stop-With "image is tagged for board '$tag', not '$BOARD_NAME'." }
} else {
  $tag = '(none)'
}

$info = Invoke-Esptool @('image-info', $Firmware)
$valid = @($info.Output | Where-Object { $_ -match '\(valid\)' }).Count
if ($info.Code -ne 0 -or $valid -lt 2) {
  $info.Output | ForEach-Object { Write-Host $_ }
  Stop-With 'esptool did not confirm both the checksum and the SHA-256 of the image.'
}

$versionMatch = [regex]::Match($text, '\d+\.\d+\.\d+-borges\.\d+[\x21-\x7E]*')
$version = if ($versionMatch.Success) { $versionMatch.Value } else { 'unknown' }
$sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Firmware).Hash.ToLowerInvariant()

# --- Port ----------------------------------------------------------------------
$portNote = ''
if (-not $Port) {
  $ports = Find-UsbPorts
  if ($ports.Count -eq 1) {
    $Port = $ports[0]
  } elseif ($ports.Count -gt 1) {
    Stop-With "several Espressif devices connected ($($ports -join ', ')). Choose one with -Port."
  } elseif ($DryRun) {
    $Port = 'COM?'
    $portNote = ' (no reader detected now)'
  } else {
    Stop-With 'no reader detected. Turn it on (not asleep), use a data USB cable, or pass -Port.'
  }
}

$writeArgs = @('--chip', 'esp32c3', '--port', $Port, '--baud', '921600',
  '--before', 'default-reset', '--after', 'hard-reset',
  'write-flash', '--flash-mode', 'keep', '--flash-freq', 'keep', '--flash-size', 'keep',
  $OTADATA_OFFSET, $bootApp0, $APP0_OFFSET, $Firmware)

Write-Host "Image     : $Firmware"
Write-Host "Version   : $version"
Write-Host "Size      : $($bytes.Length) bytes (free in the slot: $($APP_SLOT_BYTES - $bytes.Length))"
Write-Host "SHA-256   : $sha256"
Write-Host "Chip/board: ESP32-C3 / $tag"
Write-Host "Port      : $Port$portNote"
Write-Host "Writes    : $APP0_OFFSET (app0) and $OTADATA_OFFSET (otadata, boot app0 next)"
Write-Host 'Keeps     : bootloader, partition table, NVS, app1 and the SD card'
if ($version -match '-dev\+') {
  Write-Host 'WARNING   : development build. Use the release build for the reader you read on.'
}
Write-Host ''
Write-Host 'Command:'
Write-Host "  `"$($script:Python)`" `"$($script:Esptool)`" $($writeArgs -join ' ')"
Write-Host ''

if ($DryRun) {
  Write-Host 'Dry run: image checks passed, the port was not opened and nothing was written.'
  exit 0
}

# --- Device checks -------------------------------------------------------------
$probe = Invoke-Esptool @('--chip', 'auto', '--port', $Port, '--before', 'default-reset', '--after', 'no-reset', 'flash-id')
$probe.Output | ForEach-Object { Write-Host $_ }
if ($probe.Code -ne 0) { Stop-With "could not talk to the reader on $Port." }
if (-not ($probe.Output | Where-Object { $_ -match 'Connected to ESP32-C3\b' })) {
  Stop-With 'the connected device is not an ESP32-C3.'
}
if (-not ($probe.Output | Where-Object { $_ -match 'Detected flash size:\s*16MB' })) {
  Stop-With 'the connected device does not report 16 MB of flash.'
}

if (-not $Yes) {
  $answer = Read-Host 'Write this image? Type yes to continue'
  if ($answer -ne 'yes') {
    Write-Host 'Cancelled. Nothing was written.'
    exit 1
  }
}

$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $script:Python $script:Esptool @writeArgs
$code = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($code -ne 0) {
  Write-Host "esptool failed (exit $code). Do not unplug; run this script again."
  exit $code
}
Write-Host 'Done. The reader restarts by itself.'
