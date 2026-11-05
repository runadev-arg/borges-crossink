<#
.SYNOPSIS
  Every row Home draws can be reached with the buttons. Regression: a literal row count was not
  raised when Borges joined the menu, so Down wrapped before Settings on the default theme.

.DESCRIPTION
  An empty card, so the list is exactly: files, library, Borges, file transfer, settings.
  Four presses of Down and Confirm must enter Settings, on each theme that uses that list.
  Exit code 0 only if all of them did.
#>
param(
  [string]$WorkRoot = 'C:\CrossInkBuild\tmp\sim\home-menu',
  [string]$Program,
  [int[]]$Themes = @(0, 1, 3)
)

$scenario = Join-Path $PSScriptRoot 'run-scenario.ps1'
$failed = 0
foreach ($theme in $Themes) {
  $seed = Join-Path $WorkRoot "seed-$theme"
  New-Item -ItemType Directory -Force (Join-Path $seed '.crosspoint') | Out-Null
  [IO.File]::WriteAllText((Join-Path $seed '.crosspoint\crossink-settings.json'), "{`"uiTheme`":$theme}")
  $work = Join-Path $WorkRoot "theme-$theme"
  $inputs = '20000:DOWN;20600:DOWN;21200:DOWN;21800:DOWN;23000:ENTER;26000:QUIT'
  $arguments = @{ Name = "home-menu-$theme"; WorkDir = $work; SeedSd = $seed; InputScript = $inputs
    Screenshots = '22600:row'; TimeoutSec = 45; HttpPort = 18083; Quiet = $true }
  if ($Program) { $arguments.Program = $Program }
  & $scenario @arguments | Out-Null
  $entered = Select-String -Path (Join-Path $work 'simulator.log') -Pattern 'Entering activity: Settings' -Quiet
  if ($entered) {
    Write-Output "theme ${theme}: Settings reached"
  } else {
    Write-Output "theme ${theme}: Settings NOT reached"
    $failed++
  }
}
exit $failed
