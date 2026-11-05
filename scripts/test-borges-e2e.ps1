# Proves the real firmware, in the Windows simulator, against the real Borges server on a
# throwaway PostgreSQL 17, with a second client that behaves as the KOReader plugin. Nothing
# reaches production: test/borges_contract/guard.mjs refuses any non-loopback socket in the
# server, and the firmware only ever gets the local URL through BORGES_SIM_BASE_URL.
#
#   powershell -ExecutionPolicy Bypass -File scripts\test-borges-e2e.ps1 [-Only 1,2] [-Keep] [-NoBuild]
#
# -NoBuild uses the program.exe already in .pio. Screenshots of a run land in
# D:\CrossInkBorges\artifacts\e2e, everything else under D:\CrossInkBorges\tmp\e2e and goes
# away at the end unless -Keep.

param(
  [string]$Only = "",
  [switch]$Keep,
  [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$Root = "D:\CrossInkBorges"
$Repo = Split-Path -Parent $PSScriptRoot
$Work = "$Root\tmp\e2e"
$Bin = "$Work\bin"
$Build = "$Repo\.pio\build\simulator-windows"
$SdlRoot = "$Root\crossink-tools\SDL2-2.32.10\x86_64-w64-mingw32"
$Pio = "$Root\crossink-tools\pio.ps1"
$PgCtl = "C:\Program Files\PostgreSQL\17\bin\pg_ctl.exe"

$env:TMP = $Work
$env:TEMP = $Work
New-Item -ItemType Directory -Force $Work, $Bin | Out-Null

foreach ($tool in @($PgCtl)) {
  if (-not (Test-Path $tool)) { throw "Missing $tool" }
}
if (-not (Get-Command clang -ErrorAction SilentlyContinue)) { throw "clang (llvm-mingw) is not on PATH" }
if (-not (Get-Command node -ErrorAction SilentlyContinue)) { throw "node is not on PATH" }

if (-not $NoBuild) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File $Pio run -e simulator-windows | Out-File "$Work\build.log" -Encoding utf8
  if ($LASTEXITCODE -ne 0) { throw "Simulator build failed; see $Work\build.log" }
}

# A private copy, so a build running elsewhere cannot change the program under a test, with
# the control seam in front of the real SDL2. program.exe itself is not modified.
Copy-Item "$Build\program.exe" "$Bin\program.exe" -Force
Copy-Item "$SdlRoot\bin\SDL2.dll" "$Bin\SDL2_real.dll" -Force
$Proxy = "$Repo\test\borges_e2e\sdlproxy"
& clang -O1 -shared -w "-I$SdlRoot\include\SDL2" "$Proxy\sdlproxy.c" "$Proxy\sdlproxy.def" -o "$Bin\SDL2.dll" -lws2_32
if ($LASTEXITCODE -ne 0) { throw "Could not build the SDL proxy" }

$RunArgs = @("$Repo\test\borges_e2e\run.mjs", "--program", "$Bin\program.exe")
if ($Only) { $RunArgs += @("--only", $Only) }
if ($Keep) { $RunArgs += "--keep" }

$ExitCode = 1
try {
  & node @RunArgs
  $ExitCode = $LASTEXITCODE
} finally {
  # The runner cleans up after itself. This is for when it was killed: only what it left in
  # its own directories, by command line, and nothing else that happens to be running.
  Get-CimInstance Win32_Process -Filter "Name = 'program.exe'" | Where-Object { $_.ExecutablePath -eq "$Bin\program.exe" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  $PidFile = "$Work\run\server.pid"
  if (Test-Path $PidFile) {
    $ServerPid = [int](Get-Content $PidFile)
    $Process = Get-Process -Id $ServerPid -ErrorAction SilentlyContinue
    if ($Process -and $Process.ProcessName -eq "node") {
      $Line = (Get-CimInstance Win32_Process -Filter "ProcessId = $ServerPid").CommandLine
      if ($Line -match "borges_contract") { Stop-Process -Id $ServerPid -Force }
    }
  }
  if (Test-Path "$Work\pg\postmaster.pid") {
    & $PgCtl --pgdata="$Work\pg" --mode=fast --wait stop | Out-Null
  }
}
exit $ExitCode
