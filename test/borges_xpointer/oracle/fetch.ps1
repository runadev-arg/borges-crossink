# Fetches, once, what cre-oracle compiles: nothing is vendored and no script from them is run.
# Pins: koreader v2026.07.1 -> koreader-base 6e4bc81af4e04f78d81677a323a780a71b29702a
#   -> thirdparty/kpvcrlib/crengine b32a88ffbbf505cf2ea5208c1a49002d70d3609f
# Dependency URLs and md5sums are the ones in koreader-base thirdparty/<dep>/CMakeLists.txt.
param([string]$Dest = 'D:\CrossInkBorges\crossink-tools\crengine')
$ErrorActionPreference = 'Stop'
$crengine = 'b32a88ffbbf505cf2ea5208c1a49002d70d3609f'
$deps = @(
  @{ File = 'zlib.tar.xz'; Dir = 'zlib-1.3.2'; Md5 = 'be8a129ec5d60c5084911fd7ea16d84e'; Url = 'https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz' },
  @{ File = 'freetype.tar.gz'; Dir = 'freetype-VER-2-14-3'; Md5 = '0f816defb4d1d7dd274df0937431916f'; Url = 'https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz' },
  @{ File = 'xxhash.tar.gz'; Dir = 'xxHash-0.8.3'; Md5 = '599804eb9555e51c05f1b821f9212a07'; Url = 'https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz' }
)
New-Item -ItemType Directory -Force $Dest | Out-Null
$src = Join-Path $Dest 'src'
if (-not (Test-Path (Join-Path $src '.git'))) {
  git init -q $src
  git -C $src remote add origin https://github.com/koreader/crengine.git
}
$head = ''
try { $head = (git -C $src rev-parse HEAD 2>$null) } catch { }
if ($head -ne $crengine) {
  git -C $src -c advice.detachedHead=false fetch -q --depth 1 origin $crengine
  git -C $src -c advice.detachedHead=false checkout -q FETCH_HEAD
}
$depDir = Join-Path $Dest 'deps'
New-Item -ItemType Directory -Force $depDir | Out-Null
foreach ($d in $deps) {
  if (Test-Path (Join-Path $depDir $d.Dir)) { continue }
  $archive = Join-Path $depDir $d.File
  & curl.exe -sSL -o $archive $d.Url
  $md5 = (Get-FileHash $archive -Algorithm MD5).Hash.ToLower()
  if ($md5 -ne $d.Md5) { throw "$($d.File): md5 $md5, expected $($d.Md5)" }
  & tar.exe -xf $archive -C $depDir
  if ($LASTEXITCODE) { throw "could not unpack $($d.File)" }
}
$global:LASTEXITCODE = 0
