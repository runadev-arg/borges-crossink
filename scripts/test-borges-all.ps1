# Every Borges gate, in order, against the tree this script belongs to.
# Exits nonzero if any gate fails; prints one line per gate at the end.
#   powershell -ExecutionPolicy Bypass -File scripts\test-borges-all.ps1 [-Skip e2e,xpointer] [-Server <checkout>]
#
# -Server (or BORGES_KOBO) is the checkout of the Borges server the contract and e2e gates run. By
# default the main one; a fix on its way to production has to pass too. Whichever it is, its .env is
# never read: the guard in test/borges_contract keeps those runs on a throwaway database.
param([string]$Skip = "", [string]$Server = "")
$Repo = Split-Path -Parent $PSScriptRoot
$env:BORGES_PROJECT = $Repo
if ($Server) {
  if (-not (Test-Path "$Server\server\index.js")) { throw "No Borges server at $Server" }
  $env:BORGES_KOBO = $Server
}
if ($env:BORGES_KOBO) { Write-Output "Server under test: $env:BORGES_KOBO" } else { Write-Output "Server under test: the main checkout" }
# Host test output must not share a CMake cache with another checkout.
if (-not $env:BORGES_TMP) { $env:BORGES_TMP = Join-Path (Split-Path -Parent $Repo) 'tmp\gates' }
New-Item -ItemType Directory -Force $env:BORGES_TMP | Out-Null
$skipList = $Skip -split ',' | Where-Object { $_ }
$results = @()
foreach ($gate in 'core', 'net', 'glue', 'xpointer', 'contract', 'e2e') {
  if ($skipList -contains $gate) { $results += "$gate SKIPPED"; continue }
  $script = Join-Path $PSScriptRoot "test-borges-$gate.ps1"
  $started = Get-Date
  & powershell -NoProfile -ExecutionPolicy Bypass -File $script
  $code = $LASTEXITCODE
  $minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
  $word = if ($code -eq 0) { 'PASS' } else { "FAIL (exit $code)" }
  $results += "$gate $word in $minutes min"
}
Write-Output ''
Write-Output '=== Borges gates ==='
$results | ForEach-Object { Write-Output $_ }
if ($results -match 'FAIL') { exit 1 }
exit 0
