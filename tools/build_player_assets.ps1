param([string]$ClientJar = (Join-Path $env:APPDATA '.minecraft/versions/1.21.11/1.21.11.jar'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Drawing
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $root 'assets/imported/compiled'
$archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ClientJar).Path)
$atlas = [Drawing.Bitmap]::new(256, 256, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$records = [Collections.Generic.List[object]]::new()
$seen = [Collections.Generic.HashSet[int]]::new()
try
{
    $reader = [IO.StreamReader]::new($archive.GetEntry('assets/minecraft/font/include/default.json').Open())
    try { $providers = ($reader.ReadToEnd() | ConvertFrom-Json).providers } finally { $reader.Dispose() }
    foreach ($provider in $providers)
    {
        if ($provider.type -ne 'bitmap') { continue }
        $entry = $archive.GetEntry('assets/minecraft/textures/' + $provider.file.Replace('minecraft:', ''))
        $stream = $entry.Open()
        $bitmap = [Drawing.Bitmap]::new($stream)
        try
        {
            $cellWidth = [int]($bitmap.Width / 16)
            $cellHeight = [int]($bitmap.Height / $provider.chars.Count)
            if ($cellHeight -ne 8 -or $cellWidth -ne 8) { continue }
            for ($row = 0; $row -lt $provider.chars.Count; ++$row)
            {
                $characters = [Collections.Generic.List[int]]::new()
                $line = $provider.chars[$row]
                for ($offset = 0; $offset -lt $line.Length; ++$offset)
                {
                    if ([char]::IsHighSurrogate($line[$offset]) -and $offset + 1 -lt $line.Length)
                    {
                        $characters.Add([char]::ConvertToUtf32($line[$offset], $line[$offset + 1]))
                        ++$offset
                    }
                    else { $characters.Add([int][char]$line[$offset]) }
                }
                for ($column = 0; $column -lt $characters.Count; ++$column)
                {
                    $code = $characters[$column]
                    if ($code -lt 32 -or ($code -gt 0x4ff -and ($code -lt 0x2000 -or $code -gt 0x27ff)) -or !$seen.Add($code)) { continue }
                    if ($records.Count -ge 1024) { throw 'Player font atlas is full' }
                    $targetX = ($records.Count % 32) * 8
                    $targetY = [int][Math]::Floor($records.Count / 32) * 8
                    $width = 0
                    for ($y = 0; $y -lt 8; ++$y)
                    {
                        for ($x = 0; $x -lt 8; ++$x)
                        {
                            $pixel = $bitmap.GetPixel($column * 8 + $x, $row * 8 + $y)
                            $atlas.SetPixel($targetX + $x, $targetY + $y, $pixel)
                            if ($pixel.A -gt 16) { $width = [Math]::Max($width, $x + 1) }
                        }
                    }
                    $advance = if ($code -eq 32) { 4 } else { $width + 1 }
                    $records.Add(@($code, $targetX, $targetY, [Math]::Max(1,$width), $advance))
                }
            }
        }
        finally { $bitmap.Dispose(); $stream.Dispose() }
    }
    $atlas.SetPixel(255,255,[Drawing.Color]::White)
    $atlas.Save((Join-Path $output 'player-font.png'), [Drawing.Imaging.ImageFormat]::Png)
    $writer = [IO.BinaryWriter]::new([IO.File]::Create((Join-Path $output 'player-font.bin')))
    try
    {
        $writer.Write([uint32]$records.Count)
        foreach ($record in $records)
        {
            $writer.Write([uint32]$record[0])
            $writer.Write([uint16]$record[1])
            $writer.Write([uint16]$record[2])
            $writer.Write([byte]$record[3])
            $writer.Write([byte]$record[4])
        }
    }
    finally { $writer.Dispose() }
}
finally { $atlas.Dispose(); $archive.Dispose() }
Copy-Item -LiteralPath (Join-Path $root 'assets/imported/assets/minecraft/textures/entity/player/wide/steve.png') -Destination (Join-Path $output 'steve.png') -Force
Copy-Item -LiteralPath (Join-Path $root 'assets/imported/assets/minecraft/textures/entity/player/slim/alex.png') -Destination (Join-Path $output 'alex.png') -Force
foreach ($name in @('villager', 'wandering_trader', 'zombie_villager', 'armorstand')) {
    Copy-Item -LiteralPath (Join-Path $root "assets/imported/assets/minecraft/textures/entity/$name/$name.png") -Destination (Join-Path $output "$name.png") -Force
}
Write-Output ("Prepared player skins and {0} bitmap glyphs" -f $records.Count)
