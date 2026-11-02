$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$cmake = Get-ChildItem -Path (Join-Path $root 'tools') -Filter 'cmake.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $cmake) {
    throw 'cmake not found under tools/; install CMake or put it in tools/'
}

$mingw = Get-ChildItem -Path (Join-Path $root 'tools') -Filter 'gcc.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $mingw) {
    throw 'gcc not found under tools/; install MinGW-w64 or put it in tools/mingw64'
}

$env:PATH = (Split-Path $mingw.FullName) + ';' + $env:PATH
$build = Join-Path $root 'build'
$tmp = Join-Path $root '.tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp

& $cmake.FullName -E env "TMP=$tmp" "TEMP=$tmp" "TMPDIR=$tmp" `
    $cmake.FullName -S $root -B $build -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=RelWithDebInfo
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

& $cmake.FullName -E env "TMP=$tmp" "TEMP=$tmp" "TMPDIR=$tmp" `
    $cmake.FullName --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

Write-Host ''
Write-Host ('built: ' + (Join-Path $build 'netdebug.exe'))
