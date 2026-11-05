# Proves lib/BorgesCore against the real Borges server, on a throwaway PostgreSQL 17, with
# a second client that behaves as the KOReader plugin. Nothing goes to C: and nothing
# reaches production: test/borges_contract/harness.mjs starts the server with a closed
# environment and test/borges_contract/guard.mjs refuses any non-loopback socket.
#
#   powershell -ExecutionPolicy Bypass -File scripts\test-borges-contract.ps1 [-Only 1,2] [-Keep]

param(
  [string]$Only = "",
  [switch]$Keep
)

$ErrorActionPreference = "Stop"

$Root = "D:\CrossInkBorges"
$Repo = Split-Path -Parent $PSScriptRoot
$BuildDir = "$Root\tmp\contract-build"
$Cluster = "$Root\tmp\contract-pg"
$RunDir = "$Root\tmp\contract-run"
$CMake = "$Root\pio-core\tools\tool-cmake\bin\cmake.exe"
$Ninja = "$Root\pio-core\tools\tool-ninja\ninja.exe"
$PgCtl = "C:\Program Files\PostgreSQL\17\bin\pg_ctl.exe"

$env:TMP = "$Root\tmp"
$env:TEMP = "$Root\tmp"

foreach ($tool in @($CMake, $Ninja, $PgCtl)) {
  if (-not (Test-Path $tool)) { throw "Missing $tool" }
}
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) { throw "clang++ (llvm-mingw) is not on PATH" }
if (-not (Get-Command node -ErrorAction SilentlyContinue)) { throw "node is not on PATH" }

$NinjaArg = $Ninja -replace '\\', '/'
& $CMake -S "$Repo\test\borges_contract" -B $BuildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$NinjaArg" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug | Out-Null
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
& $CMake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$RunArgs = @("$Repo\test\borges_contract\run.mjs", "--exe", "$BuildDir\borges-cli.exe")
if ($Only) { $RunArgs += @("--only", $Only) }
if ($Keep) { $RunArgs += "--keep" }

$ExitCode = 1
try {
  & node @RunArgs
  $ExitCode = $LASTEXITCODE
} finally {
  # The runner cleans up after itself. This is for when it was killed: only what it left
  # in these fixed paths, and nothing else that happens to be running.
  $PidFile = "$RunDir\server.pid"
  if (Test-Path $PidFile) {
    $ServerPid = [int](Get-Content $PidFile)
    $Process = Get-Process -Id $ServerPid -ErrorAction SilentlyContinue
    if ($Process -and $Process.ProcessName -eq "node") {
      $Line = (Get-CimInstance Win32_Process -Filter "ProcessId = $ServerPid").CommandLine
      if ($Line -match "borges_contract") { Stop-Process -Id $ServerPid -Force }
    }
  }
  if (Test-Path "$Cluster\postmaster.pid") {
    & $PgCtl --pgdata=$Cluster --mode=fast --wait stop | Out-Null
  }
  if (-not $Keep) {
    Remove-Item -LiteralPath $Cluster -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $RunDir -Recurse -Force -ErrorAction SilentlyContinue
  }
}
exit $ExitCode
