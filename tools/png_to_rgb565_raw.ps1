# Converte PNG para RGB565 little-endian (compativel com guara565.raw do projeto)
param(
  [string]$InputPath,
  [string]$OutputPath,
  [int]$Width = 200,
  [int]$Height = 218
)

Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile((Resolve-Path $InputPath))
$bmp = New-Object System.Drawing.Bitmap $Width, $Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$g.DrawImage($src, 0, 0, $Width, $Height)
$g.Dispose()
$src.Dispose()

$ms = New-Object System.IO.MemoryStream
for ($y = 0; $y -lt $Height; $y++) {
  for ($x = 0; $x -lt $Width; $x++) {
    $c = $bmp.GetPixel($x, $y)
    $r = [int]$c.R
    $gn = [int]$c.G
    $b = [int]$c.B
    $rgb565 = ((($r -band 0xF8) -shl 8) -bor (($gn -band 0xFC) -shl 3) -bor ($b -shr 3))
    $lo = $rgb565 -band 0xFF
    $hi = ($rgb565 -shr 8) -band 0xFF
    $ms.WriteByte([byte]$lo)
    $ms.WriteByte([byte]$hi)
  }
}
$bmp.Dispose()

[System.IO.File]::WriteAllBytes($OutputPath, $ms.ToArray())
$ms.Dispose()
Write-Host ("OK: {0} bytes -> {1}" -f ((Get-Item $OutputPath).Length), $OutputPath)
