param([switch]$ReuseRaylib)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sdk = Join-Path $root '.tools\pspdev'
$raylib = Join-Path $root '.deps\raylib-psp-6\src'
$out = Join-Path $root 'build\psp'
if (!(Test-Path (Join-Path $sdk 'bin\psp-gcc.exe')) -or !(Test-Path (Join-Path $raylib 'platforms\rcore_psp.c'))) { throw 'PSP SDK or Raylib 6.0 PSP source is missing.' }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PSPDEV = $sdk
$env:Path = "$(Join-Path $sdk 'bin');$env:Path"
$sourcePaths = "-fmacro-prefix-map=$root=."
$includes = @('-I', (Join-Path $root 'src'), '-I', $raylib, '-I', (Join-Path $sdk 'psp\include'), '-I', (Join-Path $sdk 'psp\sdk\include'))
$objects = @()
foreach ($name in @('rcore','rshapes','rtextures','rtext','rmodels')) {
    $object = Join-Path $out "raylib_$name.o"
    if (!$ReuseRaylib -or !(Test-Path -LiteralPath $object)) {
        & psp-gcc.exe -O2 -G0 $sourcePaths -DPLATFORM_PSP -DGRAPHICS_API_OPENGL_11 -DSUPPORT_MODULE_RAUDIO=0 -DSUPPORT_FILEFORMAT_M3D=0 -DSUPPORT_FILEFORMAT_GLTF=0 -DSUPPORT_FILEFORMAT_OBJ=0 -DSUPPORT_FILEFORMAT_IQM=0 -DSUPPORT_FILEFORMAT_VOX=0 @includes -c (Join-Path $raylib "$name.c") -o $object
        if ($LASTEXITCODE -ne 0) { throw "PSP Raylib compilation failed: $name" }
    }
    $objects += $object
}
foreach ($name in @('main','world','protocol','net','offline_uuid','chunk','session','render','render_psp','movement','chat','servers','browser','players','skin_cache','avatars','inventory','game_ui','play_effects','scoreboard','pause_menu')) {
    $object = Join-Path $out "game_$name.o"
    & psp-g++.exe -std=c++17 -O2 -G0 $sourcePaths -fno-math-errno -Wall -Wextra -Wno-missing-field-initializers @includes -c (Join-Path $root "src\$name.cpp") -o $object
    if ($LASTEXITCODE -ne 0) { throw "PSP game compilation failed: $name" }
    $objects += $object
}
$elf = Join-Path $out 'CraftNet.elf'
& psp-g++.exe '-Wl,-zmax-page-size=128' $objects -L (Join-Path $sdk 'psp\lib') -L (Join-Path $sdk 'psp\sdk\lib') -lpng -lz -lglut -lGLU -lGL -lpspvfpu -lpspgum -lpspgu -lpspaudio -lpspaudiolib -lpsppower -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpspnet -lpspnet_apctl -o $elf
if ($LASTEXITCODE -ne 0) { throw 'PSP link failed.' }
$fixupOutput = @(& psp-fixup-imports.exe $elf 2>&1)
$fixupOutput | Write-Output
if ($LASTEXITCODE -ne 0 -or ($fixupOutput -join "`n") -match 'could not fixup imports') { throw 'PSP import fixup failed.' }
$sfo = Join-Path $out 'PARAM.SFO'
& mksfoex.exe -d 'MEMSIZE=1' 'CraftNet' $sfo
if ($LASTEXITCODE -ne 0) { throw 'PSP SFO creation failed.' }
$stripped = Join-Path $out 'CraftNet-stripped.elf'
& psp-strip.exe $elf -o $stripped
if ($LASTEXITCODE -ne 0) { throw 'PSP stripping failed.' }
& pack-pbp.exe (Join-Path $out 'EBOOT.PBP') $sfo NULL NULL NULL NULL NULL $stripped NULL
if ($LASTEXITCODE -ne 0) { throw 'PSP package creation failed.' }
$assetDestination = Join-Path $out 'assets\imported\compiled'
New-Item -ItemType Directory -Force -Path $assetDestination | Out-Null
Copy-Item -LiteralPath (Get-ChildItem -LiteralPath (Join-Path $root 'assets\imported\compiled') -File | Where-Object { $_.Name -match '^blocks[0-9]*\.png$|^blocks\.bin$|^chat\.ttf$|^steve\.png$|^alex\.png$|^player-font\.(png|bin)$|^(villager|wandering_trader|zombie_villager|armorstand)\.png$|^inventory-(gui|items[0-9]+|armor)\.png$|^inventory-items\.bin$' } | Select-Object -ExpandProperty FullName) -Destination $assetDestination -Force
if (!(Test-Path (Join-Path $out 'server.cfg'))) { Copy-Item (Join-Path $root 'server.cfg.example') (Join-Path $out 'server.cfg') }
Write-Output "Built $out\EBOOT.PBP"
