$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
foreach ($platform in @('psp','windows')) {
    $source = Join-Path $root "build\$platform"
    $name = if ($platform -eq 'psp') { 'CraftNet-PSP' } else { 'CraftNet-Windows' }
    $program = if ($platform -eq 'psp') { 'EBOOT.PBP' } else { 'CraftNet.exe' }
    $destination = Join-Path $root "dist\$name"
    if (!(Test-Path (Join-Path $source $program))) { throw "Missing $program; build $platform first." }
    New-Item -ItemType Directory -Force -Path (Join-Path $destination 'assets\imported\compiled') | Out-Null
    Copy-Item (Join-Path $source $program) (Join-Path $destination $program) -Force
    Copy-Item -LiteralPath (Get-ChildItem -LiteralPath (Join-Path $source 'assets\imported\compiled') -File | Where-Object { $_.Name -match '^blocks[0-9]*\.png$|^blocks\.bin$|^chat\.ttf$|^steve\.png$|^alex\.png$|^player-font\.(png|bin)$|^(villager|wandering_trader|zombie_villager|armorstand)\.png$|^inventory-(gui|items[0-9]+|armor)\.png$|^inventory-items\.bin$' } | Select-Object -ExpandProperty FullName) (Join-Path $destination 'assets\imported\compiled') -Force
    if (!(Test-Path (Join-Path $destination 'server.cfg'))) { Copy-Item (Join-Path $source 'server.cfg') (Join-Path $destination 'server.cfg') }
}
Write-Output 'Prepared dist/CraftNet-PSP and dist/CraftNet-Windows.'
