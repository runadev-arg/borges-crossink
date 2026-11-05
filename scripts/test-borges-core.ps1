# Configures, builds and runs test/borges_core natively on Windows, with nothing else
# from the test tree. Build output and temp files stay on D:, since C: has little room.
#
#   powershell -ExecutionPolicy Bypass -File scripts\test-borges-core.ps1 [-Filter <gtest>] [-Werror] [-RiscV]
#
# -RiscV also compiles lib/BorgesCore with the firmware's toolchain and flags and prints
# the object sizes.

param(
  [string]$Filter = "",
  [switch]$Werror,
  [switch]$RiscV
)

$ErrorActionPreference = "Stop"

$Root = "D:\CrossInkBorges"
$Repo = Split-Path -Parent $PSScriptRoot
# BORGES_TMP moves build output, for a second worktree building at the same time.
$Tmp = if ($env:BORGES_TMP) { $env:BORGES_TMP } else { "$Root\tmp" }
$BuildDir = "$Tmp\core-tests"
$CMake = "$Root\pio-core\tools\tool-cmake\bin\cmake.exe"
$Ninja = "$Root\pio-core\tools\tool-ninja\ninja.exe"
$Gpp = "$Root\pio-core\tools\toolchain-riscv32-esp\bin\riscv32-esp-elf-g++.exe"
$Size = "$Root\pio-core\tools\toolchain-riscv32-esp\bin\riscv32-esp-elf-size.exe"

New-Item -ItemType Directory -Force $Tmp | Out-Null
$env:TMP = $Tmp
$env:TEMP = $Tmp

foreach ($tool in @($CMake, $Ninja)) {
  if (-not (Test-Path $tool)) { throw "Missing $tool" }
}
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) { throw "clang++ (llvm-mingw) is not on PATH" }

$NinjaArg = $Ninja -replace '\\', '/'
$Configure = @(
  "-S", "$Repo\test", "-B", $BuildDir, "-G", "Ninja",
  "-DCMAKE_MAKE_PROGRAM=$NinjaArg",
  "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
  "-DCMAKE_BUILD_TYPE=Debug",
  "-DBORGES_CORE_ONLY=ON",
  "-DBORGES_CORE_WERROR=$(if ($Werror) { 'ON' } else { 'OFF' })",
  "-DFETCHCONTENT_BASE_DIR=$($BuildDir -replace '\\', '/')/_deps"
)
$LocalJson = "$Repo\.pio\libdeps\default\ArduinoJson"
if (Test-Path "$LocalJson\src\ArduinoJson.h") {
  # Same 7.4.2 the firmware pins: saves a download.
  $Configure += "-DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=$($LocalJson -replace '\\', '/')"
}

& $CMake @Configure
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& $CMake --build $BuildDir --target borges_core
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$Exe = "$BuildDir\borges_core\borges_core.exe"
$TestArgs = @("--gtest_brief=1")
if ($Filter) { $TestArgs += "--gtest_filter=$Filter" }
& $Exe @TestArgs
$TestExit = $LASTEXITCODE

if ($RiscV) {
  if (-not (Test-Path $Gpp)) { throw "Missing $Gpp" }
  $ObjDir = "$Tmp\core-riscv"
  New-Item -ItemType Directory -Force $ObjDir | Out-Null
  $Objects = @()
  foreach ($src in Get-ChildItem "$Repo\lib\BorgesCore\*.cpp") {
    $obj = "$ObjDir\$($src.BaseName).o"
    & $Gpp -c -std=gnu++2a -fno-exceptions -fno-rtti -Os -ffunction-sections -fdata-sections -Wall -Wextra `
      -march=rv32imc_zicsr_zifencei -mabi=ilp32 "-I$Repo\lib\BorgesCore" $src.FullName -o $obj
    if ($LASTEXITCODE -ne 0) { throw "RISC-V compile failed: $($src.Name)" }
    $Objects += $obj
  }
  & $Size -t @Objects
}

exit $TestExit
