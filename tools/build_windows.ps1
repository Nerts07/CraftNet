$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out = Join-Path $root 'build\windows'
$zlib = Join-Path $root '.deps\zlib'
$raylib = Join-Path $root '.tools\raylib-6.0_win64_mingw-w64'
New-Item -ItemType Directory -Force -Path $out | Out-Null
if (!(Test-Path (Join-Path $zlib 'zlib.h')) -or !(Test-Path (Join-Path $raylib 'lib\libraylib.a'))) { throw 'zlib source or Windows Raylib 6.0 is missing.' }
$objects = @()
foreach ($name in @('adler32','compress','crc32','deflate','gzclose','gzlib','gzread','gzwrite','infback','inffast','inflate','inftrees','trees','uncompr','zutil')) {
    $object = Join-Path $out "$name.o"
    & gcc.exe -O2 -I $zlib -c (Join-Path $zlib "$name.c") -o $object
    if ($LASTEXITCODE -ne 0) { throw "zlib compilation failed: $name" }
    $objects += $object
}
$sources = @('main','world','protocol','net','offline_uuid','chunk','session','render','render_psp','movement','chat','servers','browser','players','skin_cache','avatars','inventory','game_ui','play_effects','scoreboard','pause_menu') | ForEach-Object { Join-Path $root "src\$_.cpp" }
& g++.exe -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -static -I (Join-Path $raylib 'include') -I $zlib $sources $objects (Join-Path $raylib 'lib\libraylib.a') -o (Join-Path $out 'CraftNet.exe') -lopengl32 -lgdi32 -lwinmm -lws2_32
if ($LASTEXITCODE -ne 0) { throw 'Windows build failed.' }
$assetDestination = Join-Path $out 'assets\imported\compiled'
New-Item -ItemType Directory -Force -Path $assetDestination | Out-Null
Copy-Item -LiteralPath (Get-ChildItem -LiteralPath (Join-Path $root 'assets\imported\compiled') -File | Where-Object { $_.Name -match '^blocks[0-9]*\.png$|^blocks\.bin$|^chat\.ttf$|^steve\.png$|^alex\.png$|^player-font\.(png|bin)$|^(villager|wandering_trader|zombie_villager|armorstand)\.png$|^inventory-(gui|items[0-9]+|armor)\.png$|^inventory-items\.bin$' } | Select-Object -ExpandProperty FullName) -Destination $assetDestination -Force
if (!(Test-Path (Join-Path $out 'server.cfg'))) { Copy-Item (Join-Path $root 'server.cfg.example') (Join-Path $out 'server.cfg') }
Write-Output "Built $out\CraftNet.exe"
