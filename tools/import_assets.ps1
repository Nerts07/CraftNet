param(
    [Parameter(Mandatory=$true)][string]$ClientJar,
    [string]$Output = (Join-Path $PSScriptRoot '..\assets\imported')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = [IO.Path]::GetFullPath($Output).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ClientJar).Path)
try {
    $files = @()
    $total = [long]0
    foreach ($entry in $archive.Entries) {
        $name = $entry.FullName
        if ($name -notmatch '^assets/[^/]+/(textures/.+\.(png|png\.mcmeta)|models/.+\.json|blockstates/.+\.json)$') { continue }
        if ($name.Contains('\') -or $name.Contains(':') -or ($name.Split('/') -contains '..') -or $entry.Length -gt 16MB) { throw "Unsafe resource: $name" }
        $total += $entry.Length
        if ($total -gt 512MB) { throw 'Selected resources exceed the extraction limit.' }
        $dest = [IO.Path]::GetFullPath((Join-Path $root $name.Replace('/', [IO.Path]::DirectorySeparatorChar)))
        if (-not $dest.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "Invalid resource path: $name" }
        $files += [PSCustomObject]@{ Entry = $entry; Destination = $dest }
    }
    foreach ($file in $files) {
        $parent = [IO.Path]::GetDirectoryName($file.Destination)
        [IO.Directory]::CreateDirectory($parent) | Out-Null
        # Refuse to follow existing junctions or symbolic links in the output tree.
        $dir = Get-Item -LiteralPath $parent -Force
        while ($dir.FullName.Length -ge $root.TrimEnd('\').Length) {
            if ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Output path contains a link: $($dir.FullName)" }
            if ($dir.FullName -eq $root.TrimEnd('\')) { break }
            $dir = $dir.Parent
        }
        if ((Test-Path -LiteralPath $file.Destination) -and ((Get-Item -LiteralPath $file.Destination -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Output file is a link.' }
        $source = $file.Entry.Open()
        try {
            $destStream = [IO.File]::Open($file.Destination, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $source.CopyTo($destStream) } finally { $destStream.Dispose() }
        } finally { $source.Dispose() }
    }
    $textureCount = @($files | Where-Object { $_.Entry.FullName -match '^assets/[^/]+/textures/.+\.png$' }).Count
    Write-Output "Extracted $($files.Count) files including $textureCount PNG textures ($total bytes) to $root"
} finally { $archive.Dispose() }
