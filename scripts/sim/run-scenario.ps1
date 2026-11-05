<#
.SYNOPSIS
  Runs the native Windows simulator through one scripted scenario, unattended.

.DESCRIPTION
  Sets up a fresh simulated SD card, runs program.exe with no visible window, kills
  it at the timeout, turns the BMP screenshots into PNGs and exits with a code a
  test stage can act on. The whole run stays under -WorkDir, including temp files.

  Times in -InputScript and -Screenshots are milliseconds since process start.
  A screenshot entry is <ms>:<name>; <name> becomes <WorkDir>\shots\<name>.bmp/.png.

.EXAMPLE
  scripts\sim\run-scenario.ps1 -Name reader -Books test\epubs\test_reader_rendering_matrix.epub `
    -InputScript '3000:ENTER;9000:DOWN;11000:BACK;14000:QUIT' `
    -Screenshots '2500:home;8000:open;10000:page2;13000:back' -TimeoutSec 60

.OUTPUTS
  Exit code: the simulator's own exit code when it ended by itself, otherwise
    124  timed out and was killed
    125  bad arguments or the program is missing
    126  a screenshot that was asked for does not exist
  The log is at <WorkDir>\simulator.log.
#>
[CmdletBinding()]
param(
  # Run name: the default work directory is D:\CrossInkBorges\tmp\sim\<Name>.
  [string]$Name = 'scenario',
  # Run directory. It is emptied first unless -KeepSd is set.
  [string]$WorkDir,
  # Program to run. Default: the simulator-windows build of this repository.
  [string]$Program,
  # EPUBs and other files to copy into /books on the simulated SD.
  [string[]]$Books = @(),
  # Directory copied over the SD root first, for prepared state or settings.
  [string]$SeedSd,
  [string]$InputScript,
  [string]$Screenshots,
  [string]$InputScriptAfterReboot,
  [string]$ScreenshotsAfterReboot,
  [string]$InputScriptAfterWake,
  [string]$ScreenshotsAfterWake,
  # Extra environment variables for the simulator.
  [hashtable]$Env = @{},
  [int]$TimeoutSec = 60,
  # Port of the firmware's web server; the WebSocket server takes the next one.
  [int]$HttpPort = 18080,
  # Keep the SD from the previous run under this WorkDir (for multi-stage tests).
  [switch]$KeepSd,
  # Show the SDL window instead of using SDL's dummy video driver.
  [switch]$Window,
  # Do not print the log tail when the run fails.
  [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Fail-Setup([string]$message) {
  [Console]::Error.WriteLine("run-scenario: $message")
  exit 125
}

if (-not $Program) {
  $Program = Join-Path $repoRoot '.pio\build\simulator-windows\program.exe'
}
if (-not (Test-Path -LiteralPath $Program)) {
  Fail-Setup "program not found: $Program (build it with: pio.ps1 run -e simulator-windows)"
}
$Program = (Resolve-Path -LiteralPath $Program).Path

if (-not $WorkDir) { $WorkDir = Join-Path 'D:\CrossInkBorges\tmp\sim' $Name }
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$sdDir = Join-Path $WorkDir 'sd'
$shotsDir = Join-Path $WorkDir 'shots'
$tempDir = Join-Path $WorkDir 'temp'
$logFile = Join-Path $WorkDir 'simulator.log'

if ((Test-Path -LiteralPath $WorkDir) -and -not $KeepSd) {
  Remove-Item -LiteralPath $WorkDir -Recurse -Force
}
foreach ($dir in @($sdDir, $shotsDir, $tempDir, (Join-Path $sdDir 'books'))) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
if ($KeepSd) {
  Get-ChildItem -LiteralPath $shotsDir -File | Remove-Item -Force
}

if ($SeedSd) {
  Copy-Item -Path (Join-Path $SeedSd '*') -Destination $sdDir -Recurse -Force
}
foreach ($book in $Books) {
  $source = if ([IO.Path]::IsPathRooted($book)) { $book } else { Join-Path $repoRoot $book }
  if (-not (Test-Path -LiteralPath $source)) { Fail-Setup "book not found: $source" }
  Copy-Item -LiteralPath $source -Destination (Join-Path $sdDir 'books') -Force
}

# "<ms>:<name>;..." -> "<ms>:<WorkDir>\shots\<name>.bmp;..." plus the list of files.
$expectedShots = New-Object System.Collections.Generic.List[string]
function Expand-Screenshots([string]$schedule, [string]$suffix) {
  if (-not $schedule) { return $null }
  $entries = foreach ($entry in ($schedule -split ';' | Where-Object { $_ })) {
    $parts = $entry -split ':', 2
    if ($parts.Count -ne 2 -or $parts[0] -notmatch '^\d+$') {
      Fail-Setup "bad screenshot entry '$entry' (want <ms>:<name>)"
    }
    $file = Join-Path $shotsDir ($parts[1] + $suffix + '.bmp')
    $expectedShots.Add($file)
    "$($parts[0]):$file"
  }
  return ($entries -join ';')
}

# cmd.exe does the redirection: stdout and stderr land in one file, in order, as
# raw bytes, and the log can be followed while the run is going.
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $env:ComSpec
$startInfo.Arguments = "/d /s /c `"`"$Program`" > `"$logFile`" 2>&1`""
$startInfo.WorkingDirectory = $WorkDir
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true

$vars = @{
  CROSSPOINT_SIM_SD        = $sdDir
  CROSSPOINT_SIM_HTTP_PORT = "$HttpPort"
  TMP                      = $tempDir
  TEMP                     = $tempDir
}
if (-not $Window) { $vars['SDL_VIDEODRIVER'] = 'dummy' }
if ($InputScript) { $vars['CROSSPOINT_SIM_INPUT_SCRIPT'] = $InputScript }
if ($InputScriptAfterReboot) { $vars['CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_REBOOT'] = $InputScriptAfterReboot }
if ($InputScriptAfterWake) { $vars['CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE'] = $InputScriptAfterWake }
$schedule = Expand-Screenshots $Screenshots ''
if ($schedule) { $vars['CROSSPOINT_SIM_SCREENSHOTS'] = $schedule }
$schedule = Expand-Screenshots $ScreenshotsAfterReboot ''
if ($schedule) { $vars['CROSSPOINT_SIM_SCREENSHOTS_AFTER_REBOOT'] = $schedule }
$schedule = Expand-Screenshots $ScreenshotsAfterWake ''
if ($schedule) { $vars['CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE'] = $schedule }
foreach ($key in $Env.Keys) { $vars[$key] = [string]$Env[$key] }
foreach ($key in $vars.Keys) { $startInfo.EnvironmentVariables[$key] = $vars[$key] }

$started = Get-Date
$process = [System.Diagnostics.Process]::Start($startInfo)

$timedOut = -not $process.WaitForExit($TimeoutSec * 1000)
if ($timedOut) {
  # Takes cmd.exe and the supervisor; the supervisor's job object takes the rest.
  & taskkill.exe /PID $process.Id /T /F 2>&1 | Out-Null
  [void]$process.WaitForExit(5000)
}

$exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
$elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)

# BMP -> PNG with System.Drawing. The BMP stays next to it.
Add-Type -AssemblyName System.Drawing
foreach ($bmp in Get-ChildItem -LiteralPath $shotsDir -Filter '*.bmp') {
  $image = [System.Drawing.Image]::FromFile($bmp.FullName)
  try {
    $image.Save([IO.Path]::ChangeExtension($bmp.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
  }
  finally { $image.Dispose() }
}

$missing = @($expectedShots | Where-Object { -not (Test-Path -LiteralPath $_) })
foreach ($file in $missing) { Write-Output "missing screenshot: $file" }
if ($exitCode -eq 0 -and $missing.Count -gt 0) { $exitCode = 126 }

Write-Output "scenario=$Name exit=$exitCode elapsed=${elapsed}s log=$logFile shots=$shotsDir"
if ($exitCode -ne 0 -and -not $Quiet) {
  Write-Output '--- last log lines ---'
  Get-Content -LiteralPath $logFile -Tail 30 -ErrorAction SilentlyContinue
}
exit $exitCode
