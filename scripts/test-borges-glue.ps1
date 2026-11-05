# Builds and runs test/borges_glue natively on Windows, next to test/borges_core.
# Everything stays on D:, as in scripts/test-borges-core.ps1.
#
#   powershell -ExecutionPolicy Bypass -File scripts\test-borges-glue.ps1 [-Filter <gtest>]

param([string]$Filter = "")

$ErrorActionPreference = "Stop"

$Root = "D:\CrossInkBorges"
$Repo = Split-Path -Parent $PSScriptRoot
$BuildDir = "$Root\tmp\glue-tests"
$CMake = "$Root\pio-core\tools\tool-cmake\bin\cmake.exe"
$Ninja = "$Root\pio-core\tools\tool-ninja\ninja.exe"

$env:TMP = "$Root\tmp"
$env:TEMP = "$Root\tmp"

$Configure = @(
  "-S", "$Repo\test", "-B", $BuildDir, "-G", "Ninja",
  "-DCMAKE_MAKE_PROGRAM=$($Ninja -replace '\\', '/')",
  "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
  "-DCMAKE_BUILD_TYPE=Debug",
  "-DBORGES_CORE_ONLY=ON",
  "-DFETCHCONTENT_BASE_DIR=$($BuildDir -replace '\\', '/')/_deps"
)
$LocalJson = "$Repo\.pio\libdeps\default\ArduinoJson"
if (Test-Path "$LocalJson\src\ArduinoJson.h") {
  $Configure += "-DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=$($LocalJson -replace '\\', '/')"
}

& $CMake @Configure
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
& $CMake --build $BuildDir --target borges_glue
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$TestArgs = @("--gtest_brief=1")
if ($Filter) { $TestArgs += "--gtest_filter=$Filter" }
& "$BuildDir\borges_glue\borges_glue.exe" @TestArgs
exit $LASTEXITCODE
