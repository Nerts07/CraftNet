param(
    [string]$Resources = (Join-Path $PSScriptRoot '..\assets\imported'),
    [string]$BlocksJson = (Join-Path $PSScriptRoot '..\.deps\mc26-data\blocks.json')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$output = Join-Path $Resources 'compiled'
New-Item -ItemType Directory -Force -Path $output | Out-Null
if (!('CraftNetTextureInfo' -as [type]))
{
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;

public static class CraftNetTextureInfo
{
    public static bool IsOpaque(string filename)
    {
        using (Bitmap image = new Bitmap(filename))
        {
            int side = Math.Min(image.Width, image.Height);
            for (int y = 0; y < side; ++y)
            {
                for (int x = 0; x < side; ++x)
                {
                    if (image.GetPixel(x, y).A != 255)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }
}
'@
}
$opacity = @{}
foreach ($texture in (Get-ChildItem -LiteralPath (Join-Path $Resources 'assets\minecraft\textures\block') -Filter '*.png' -File))
{
    $opacity['block/' + $texture.BaseName] = [CraftNetTextureInfo]::IsOpaque($texture.FullName)
}
$encoding = New-Object Text.UTF8Encoding $false
[IO.File]::WriteAllText((Join-Path $output 'texture-opacity.json'), ($opacity | ConvertTo-Json -Compress), $encoding)
& node.exe (Join-Path $PSScriptRoot 'build_block_models.js') $Resources $BlocksJson
if ($LASTEXITCODE -ne 0)
{
    throw 'Block model compilation failed.'
}
$names = Get-Content -LiteralPath (Join-Path $output 'texture-names.json') -Raw | ConvertFrom-Json
$pages = [int][Math]::Ceiling($names.Count / 1024.0)
for ($page = 0; $page -lt $pages; ++$page)
{
    $atlas = New-Object Drawing.Bitmap 512, 512, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($atlas)
    try
    {
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
        $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
        for ($tile = 0; $tile -lt 1024 -and ($page * 1024 + $tile) -lt $names.Count; ++$tile)
        {
            $name = $names[$page * 1024 + $tile]
            $image = [Drawing.Image]::FromFile((Join-Path $Resources "assets\minecraft\textures\$name.png"))
            try
            {
                $side = [Math]::Min($image.Width, $image.Height)
                $rect = New-Object Drawing.Rectangle (($tile % 32) * 16), ([Math]::Floor($tile / 32) * 16), 16, 16
                $graphics.DrawImage($image, $rect, 0, 0, $side, $side, [Drawing.GraphicsUnit]::Pixel)
            }
            finally
            {
                $image.Dispose()
            }
        }
        $filename = if ($page -eq 0) { 'blocks.png' } else { "blocks$page.png" }
        $atlas.Save((Join-Path $output $filename), [Drawing.Imaging.ImageFormat]::Png)
    }
    finally
    {
        $graphics.Dispose()
        $atlas.Dispose()
    }
}
Write-Output "Created $pages atlas pages with 16x16 textures."
