$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$textures = Join-Path $root 'assets/imported/assets/minecraft/textures'
$out = Join-Path $root 'assets/imported/compiled'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Copy-Sprite($graphics, $file, $x, $y, $width, $height, [switch]$Crop) {
    $image = [Drawing.Bitmap]::new((Join-Path $textures "$file.png"))
    try {
        $destination = [Drawing.Rectangle]::new($x, $y, $width, $height)
        $sourceWidth = if ($Crop) { $width } else { $image.Width }
        $sourceHeight = if ($Crop) { $height } else { [Math]::Min($image.Height, $image.Width) }
        if ($file.StartsWith('gui/')) { $sourceHeight = $height }
        $graphics.DrawImage($image, $destination, 0, 0, $sourceWidth, $sourceHeight, [Drawing.GraphicsUnit]::Pixel)
    } finally { $image.Dispose() }
}

function New-AtlasGraphics($bitmap) {
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
    return $graphics
}

function Copy-Bed($graphics, $name, $x, $y) {
    # Beds have block face textures in this resource pack and no flat item sprite.
    foreach ($part in @('head', 'foot')) {
        $start = if ($part -eq 'head') { 0 } else { 6 }
        $path = Join-Path $textures "block/$($name)_$($part)_up.png"
        $image = [Drawing.Bitmap]::new($path)
        try {
            $points = [Drawing.Point[]]@(
                [Drawing.Point]::new($x + 1 + $start, $y + 5 + ($start / 2)),
                [Drawing.Point]::new($x + 7 + $start, $y + 8 + ($start / 2)),
                [Drawing.Point]::new($x + 3 + $start, $y + 2 + ($start / 2))
            )
            $graphics.DrawImage($image, $points)
        } finally { $image.Dispose() }
    }
}

$gui = [Drawing.Bitmap]::new(256, 512, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = New-AtlasGraphics $gui
try {
    Copy-Sprite $g 'gui/container/inventory' 0 0 176 166 -Crop
    Copy-Sprite $g 'gui/container/generic_54' 0 166 176 222 -Crop
    Copy-Sprite $g 'gui/sprites/hud/hotbar' 0 388 182 22 -Crop
    Copy-Sprite $g 'gui/sprites/hud/hotbar_selection' 184 388 24 23 -Crop
    Copy-Sprite $g 'gui/sprites/hud/experience_bar_background' 0 414 182 5 -Crop
    Copy-Sprite $g 'gui/sprites/hud/experience_bar_progress' 0 420 182 5 -Crop
    $x = 0
    foreach ($icon in @('heart/container','heart/half','heart/full','food_empty','food_half','food_full')) {
        Copy-Sprite $g "gui/sprites/hud/$icon" $x 428 9 9 -Crop
        $x += 10
    }
    $g.FillRectangle([Drawing.Brushes]::Magenta, 80, 428, 16, 16)
    $g.FillRectangle([Drawing.Brushes]::Black, 88, 428, 8, 8)
    $g.FillRectangle([Drawing.Brushes]::Black, 80, 436, 8, 8)
    for ($stage = 0; $stage -lt 10; ++$stage) {
        Copy-Sprite $g "block/destroy_stage_$stage" ($stage * 16) 448 16 16 -Crop
    }
    $gui.Save((Join-Path $out 'inventory-gui.png'), [Drawing.Imaging.ImageFormat]::Png)
} finally { $g.Dispose(); $gui.Dispose() }

$armor = [Drawing.Bitmap]::new(256, 128, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = New-AtlasGraphics $armor
try {
    $materials = @('leather','chainmail','iron','gold','diamond','netherite','copper','turtle_scute')
    foreach ($layer in 0,1) {
        $folder = if ($layer) { 'humanoid_leggings' } else { 'humanoid' }
        for ($i = 0; $i -lt $materials.Count; ++$i) {
            $sprite = "entity/equipment/$folder/$($materials[$i])"
            if (Test-Path -LiteralPath (Join-Path $textures "$sprite.png")) {
                Copy-Sprite $g $sprite (($i % 4) * 64) (([Math]::Floor($i / 4) + $layer * 2) * 32) 64 32
            }
        }
    }
    $armor.Save((Join-Path $out 'inventory-armor.png'), [Drawing.Imaging.ImageFormat]::Png)
} finally { $g.Dispose(); $armor.Dispose() }

$registry = (Get-Content -LiteralPath (Join-Path $root '.deps/mc26-data/registries.json') -Raw | ConvertFrom-Json).'minecraft:item'.entries
$items = @($registry.PSObject.Properties | Sort-Object { $_.Value.protocol_id })
$writer = [IO.BinaryWriter]::new([IO.File]::Create((Join-Path $out 'inventory-items.bin')))
$page = $null
$graphics = $null
$pageNumber = 0
$tile = 0
try {
    $writer.Write([uint16]$items.Count)
    foreach ($item in $items) {
        $name = $item.Name.Substring('minecraft:'.Length)
        $sprite = @("item/$name", "item/$($name)_00", "block/$name", "block/$($name)_side", "block/$($name)_front", "block/$($name)_top") |
            Where-Object { Test-Path -LiteralPath (Join-Path $textures "$_.png") } | Select-Object -First 1
        $bed = !$sprite -and $name.EndsWith('_bed') -and
            (Test-Path -LiteralPath (Join-Path $textures "block/$($name)_head_up.png")) -and
            (Test-Path -LiteralPath (Join-Path $textures "block/$($name)_foot_up.png"))
        $writer.Write([uint16]$item.Value.protocol_id)
        $writer.Write([uint16]$(if ($sprite -or $bed) { $tile } else { 65535 }))
        $writer.Write([string]$name)
        if (!$sprite -and !$bed) { continue }
        if ($tile % 1024 -eq 0) {
            if ($page) {
                $graphics.Dispose()
                $page.Save((Join-Path $out "inventory-items$pageNumber.png"), [Drawing.Imaging.ImageFormat]::Png)
                $page.Dispose()
                ++$pageNumber
            }
            $page = [Drawing.Bitmap]::new(512, 512, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $graphics = New-AtlasGraphics $page
        }
        $x = ($tile % 32) * 16
        $y = ([Math]::Floor($tile / 32) % 32) * 16
        if ($bed) { Copy-Bed $graphics $name $x $y }
        else { Copy-Sprite $graphics $sprite $x $y 16 16 }
        ++$tile
    }
    if ($page) { $page.Save((Join-Path $out "inventory-items$pageNumber.png"), [Drawing.Imaging.ImageFormat]::Png) }
} finally {
    if ($graphics) { $graphics.Dispose() }
    if ($page) { $page.Dispose() }
    $writer.Dispose()
}
Write-Output "Packed $tile item sprites and the inventory HUD."
