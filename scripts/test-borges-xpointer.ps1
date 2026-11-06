# Differential test of the KOReader position mapper against KOReader's real engine.
# Fetches crengine at the commit of the current stable KOReader, builds cre-oracle and xpdiff with
# llvm-mingw, runs every EPUB of the corpus through both and prints mismatch stats.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-borges-xpointer.ps1 [-Points 2000] [-Verbose_]
param(
  [int]$Points = 2000,
  [int]$Selections = 40,
  [switch]$Verbose_,
  [string]$Tools = 'D:\CrossInkBorges\crossink-tools\crengine',
  [string]$Build = 'C:\CrossInkBuild\cre',
  [string[]]$ExtraCorpus = @()
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$here = Join-Path $repo 'test\borges_xpointer'

& (Join-Path $here 'oracle\fetch.ps1') -Dest $Tools
if ($LASTEXITCODE) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force $Build | Out-Null
$make = @("CRE=$($Tools -replace '\\','/')/src", "DEPS=$($Tools -replace '\\','/')/deps", "OUT=$($Build -replace '\\','/')")
& mingw32-make -s -j8 -C (Join-Path $here 'oracle') @make
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$diffObj = Join-Path $Build 'xpdiff'
New-Item -ItemType Directory -Force $diffObj | Out-Null
$lib = Join-Path $repo 'lib'
$inc = @("-I$here\stubs", "-I$repo\test\chapter_html_slim_parser\stubs", "-I$lib\KOReaderSync", "-I$lib\Epub", "-I$lib\Memory", "-I$lib\MemoryBudget", "-I$lib\Utf8", "-I$lib\XmlParserUtils", "-I$lib\expat")
$cflags = @('-O1', '-g', '-w', '-DXML_CONTEXT_BYTES=1024', '-DXML_GE=0', '-Dmalloc=hmalloc', '-Drealloc=hrealloc', '-Dfree=hfree',
  '-include', "$here\stubs\host_heap.h") + $inc
foreach ($c in 'xmlparse', 'xmlrole', 'xmltok') {
  $o = "$diffObj\$c.o"
  if (-not (Test-Path $o)) { & clang @cflags -c "$lib\expat\$c.c" -o $o; if ($LASTEXITCODE) { exit 1 } }
}
$objs = @("$diffObj\xmlparse.o", "$diffObj\xmlrole.o", "$diffObj\xmltok.o")
$cxx = @('-std=c++20', '-O1', '-g', '-w', '-DXML_CONTEXT_BYTES=1024', '-DXML_GE=0', '-include', "$here\stubs\host_compat.h") + $inc
foreach ($s in @("$here\xpdiff.cpp", "$lib\KOReaderSync\ChapterXPathResolver.cpp", "$lib\KOReaderSync\ProgressMapper.cpp", "$lib\Utf8\Utf8.cpp", "$lib\Epub\Epub\htmlEntities.cpp", "$lib\Epub\Epub\css\CssParser.cpp")) {
  $o = "$diffObj\$([IO.Path]::GetFileNameWithoutExtension($s)).o"
  if (-not (Test-Path $o) -or (Get-Item $s).LastWriteTime -gt (Get-Item $o).LastWriteTime -or (Get-Item "$lib\KOReaderSync\ChapterXPathResolver.h").LastWriteTime -gt (Get-Item $o).LastWriteTime) {
    & clang++ @cxx -c $s -o $o
    if ($LASTEXITCODE) { exit 1 }
  }
  $objs += $o
}
$xpdiff = Join-Path $Build 'xpdiff.exe'
& clang++ -static -o $xpdiff @objs
if ($LASTEXITCODE) { exit 1 }

# The real parser and line breaker, with a fixed-width font: where each page really starts.
$pagesObj = Join-Path $Build 'xppages'
New-Item -ItemType Directory -Force $pagesObj | Out-Null
$parserTest = Join-Path $repo 'test\chapter_html_slim_parser'
$pagesInc = @("-I$here\stubs_parser", "-I$parserTest\stubs", "-I$lib", "-I$repo", "-I$lib\Epub", "-I$lib\Epub\Epub", "-I$lib\Epub\Epub\parsers", "-I$lib\Epub\Epub\css", "-I$lib\EpdFont", "-I$lib\Memory", "-I$lib\MemoryBudget", "-I$lib\MiniBidi", "-I$lib\Utf8", "-I$lib\XmlParserUtils", "-I$lib\Epub\Epub\tables", "-I$lib\expat", "-I$repo\src")
$pagesFlags = @('-std=c++20', '-O1', '-g', '-w', '-DXML_CONTEXT_BYTES=1024', '-DXML_GE=0', '-include', "$here\stubs\host_compat.h") + $pagesInc
$pagesObjs = @()
foreach ($s in @("$here\xppages.cpp", "$parserTest\ParserLinkStubs.cpp", "$lib\Epub\Epub\parsers\ChapterHtmlSlimParser.cpp", "$lib\Epub\Epub\css\CssParser.cpp", "$lib\Epub\Epub\ParsedText.cpp", "$lib\Epub\Epub\htmlEntities.cpp", "$lib\Utf8\Utf8.cpp", "$repo\src\clippings\ClipTextBuilder.cpp")) {
  $o = "$pagesObj\$([IO.Path]::GetFileNameWithoutExtension($s)).o"
  if (-not (Test-Path $o) -or (Get-Item $s).LastWriteTime -gt (Get-Item $o).LastWriteTime) {
    & clang++ @pagesFlags -c $s -o $o
    if ($LASTEXITCODE) { exit 1 }
  }
  $pagesObjs += $o
}
foreach ($c in 'xmlparse', 'xmlrole', 'xmltok') {
  $o = "$pagesObj\$c.o"
  if (-not (Test-Path $o)) { & clang -O1 -w -DXML_CONTEXT_BYTES=1024 -DXML_GE=0 "-I$lib\expat" -c "$lib\expat\$c.c" -o $o; if ($LASTEXITCODE) { exit 1 } }
  $pagesObjs += $o
}
$xppages = Join-Path $Build 'xppages.exe'
& clang++ -static -o $xppages @pagesObjs
if ($LASTEXITCODE) { exit 1 }
$env:XPDIFF_PAGES_TOOL = $xppages
$env:XPPAGES_SELECTIONS = "$Selections"

$corpus = Join-Path $Build 'corpus'
$work = Join-Path $Build 'work'
New-Item -ItemType Directory -Force $work | Out-Null
$sources = @((Join-Path $repo 'test\epubs')) + $ExtraCorpus
& python -I (Join-Path $here 'make_corpus.py') $corpus @sources | Out-Null
if ($LASTEXITCODE) { exit 1 }

$oracle = Join-Path $Build 'cre-oracle.exe'
if ($Verbose_) { $env:XPDIFF_VERBOSE = '1' } else { Remove-Item Env:XPDIFF_VERBOSE -ErrorAction SilentlyContinue }
$rows = @()
$pageLines = @()
foreach ($dir in Get-ChildItem $corpus -Directory) {
  $lines = & $xpdiff $dir.FullName $oracle $work $Points
  foreach ($l in $lines) {
    if ($l -match '^  (pages|clips|earlystop) ') { $pageLines += "$($dir.Name)$l"; continue }
    if ($l -notmatch "`t") { Write-Output $l; continue }
    $f = $l -split "`t"
    if ($f.Count -lt 18) { Write-Output $l; continue }
    $rows += [pscustomobject]@{
      Book = $f[0]; K2DPoints = [int]$f[1]; K2DExact = [int]$f[2]; K2DNotFound = [int]$f[3]; K2DWrong = [int]$f[4]; K2DMaxDelta = [int]$f[5]
      D2KPoints = [int]$f[6]; D2KExact = [int]$f[7]; D2KEmpty = [int]$f[8]; D2KWrong = [int]$f[9]; D2KCanonical = [int]$f[10]; D2KMaxDelta = [int]$f[11]
      RoundTrips = [int]$f[12]; RoundTripBad = [int]$f[13]; OnlyOracle = [int]$f[14]; OnlyFirmware = [int]$f[15]; PeakHeap = [int]$f[16]; PeakStack = [int]$f[17]
    }
  }
}
$rows | Format-Table Book, K2DPoints, K2DExact, K2DNotFound, K2DWrong, K2DMaxDelta, D2KPoints, D2KExact, D2KEmpty, D2KWrong, D2KCanonical, D2KMaxDelta, RoundTrips, RoundTripBad, OnlyOracle, OnlyFirmware -AutoSize | Out-String -Width 220
$sum = { param($n) ($rows | Measure-Object $n -Sum).Sum }
"K2D  points $(& $sum K2DPoints)  exact $(& $sum K2DExact)  not-found $(& $sum K2DNotFound)  wrong $(& $sum K2DWrong)"
"D2K  points $(& $sum D2KPoints)  exact $(& $sum D2KExact)  empty $(& $sum D2KEmpty)  wrong/rejected $(& $sum D2KWrong)  canonical-string $(& $sum D2KCanonical)"
$pageLines
"Round trips $(& $sum RoundTrips)  bad $(& $sum RoundTripBad)"
"Host peak heap $(($rows | Measure-Object PeakHeap -Maximum).Maximum) B, peak stack $(($rows | Measure-Object PeakStack -Maximum).Maximum) B"

# What the verdict rests on. limit_* are made to sit past what the mapper promises (entities with no
# definition, nesting past its depth, a second body), and each differs from crengine on purpose: they
# are watched for getting worse, not for being zero. Everything else has to agree exactly.
#
# "Other text" is where crengine's text at a range differs from what the reader's page shows, beyond
# spacing. What is sent is crengine's, so this is not a wrong highlight. Known: limit_entities, and one
# chapter of hostile_cases (spine 19) built from non-ASCII controls and spaces. Any more is a finding.
$badOf = { param($r) $r.K2DNotFound + $r.K2DWrong + $r.D2KEmpty + $r.D2KWrong + $r.RoundTripBad }
$limitRows = @($rows | Where-Object { $_.Book -like 'limit_*' })
$realRows = @($rows | Where-Object { $_.Book -notlike 'limit_*' })
$realBad = 0
foreach ($r in $realRows) { $realBad += & $badOf $r }
$limitBad = 0
foreach ($r in $limitRows) { $limitBad += & $badOf $r }
$knownLimitBad = 202
$knownOtherText = 88
$otherText = 0
$earlyStopChecked = 0
$earlyStopSeen = 0
foreach ($l in $pageLines) {
  if ($l -match 'otherText=(\d+)') { $otherText += [int]$Matches[1] }
  if ($l -match 'earlystop checked=(\d+) stopped=(\d+)') {
    $earlyStopChecked += [int]$Matches[1]
    $earlyStopSeen += [int]$Matches[2]
  }
}
"Corpus: $($realRows.Count) books, mismatches $realBad. Known limits: $($limitRows.Count) fixtures, mismatches $limitBad (ceiling $knownLimitBad). Other text $otherText (ceiling $knownOtherText). Early stop: $earlyStopSeen of $earlyStopChecked big chapters stopped early."
if ($rows.Count -eq 0) { "No book went through: nothing was compared."; exit 1 }
if ($earlyStopSeen -eq 0) { "No chapter stopped early."; exit 1 }
if ($realBad -or $limitBad -gt $knownLimitBad -or $otherText -gt $knownOtherText) { exit 1 }
exit 0
