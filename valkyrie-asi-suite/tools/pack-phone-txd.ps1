param([Parameter(Mandatory=$true)][string]$Source, [Parameter(Mandatory=$true)][string]$Output,
      [string[]]$RequiredNames = @())
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
# Packs every PNG in $Source into one uncompressed A8R8G8B8 texture dictionary,
# each at its own size. The same RenderWare layout as pack-inventory-icons.ps1,
# which squares everything; the phone's body, bars and buttons are not square.
function Chunk([uint32]$type,[byte[]]$data) {
    $stream=[IO.MemoryStream]::new();$writer=[IO.BinaryWriter]::new($stream)
    $writer.Write($type);$writer.Write([uint32]$data.Length);$writer.Write([uint32]0x1803FFFF);$writer.Write($data)
    $result=$stream.ToArray();$writer.Dispose();return ,$result
}
function IsPow2([int]$v) { return $v -gt 0 -and ($v -band ($v-1)) -eq 0 }
$files=@(Get-ChildItem -LiteralPath $Source -Filter *.png | Sort-Object Name)
if(!$files.Count -or $files.Count -gt 65535){throw 'Invalid texture count'}
foreach ($requiredName in $RequiredNames) {
    if (-not ($files | Where-Object { $_.BaseName -ieq $requiredName })) {
        throw "Required phone artwork is missing: $requiredName.png"
    }
}
$dict=[IO.MemoryStream]::new();$out=[IO.BinaryWriter]::new($dict)
$out.Write((Chunk 1 ([BitConverter]::GetBytes([uint16]$files.Count)+[byte[]]@(0,0))))
foreach($file in $files) {
    $name=$file.BaseName.ToLowerInvariant()
    if($name.Length -gt 31){throw "Texture name too long for RenderWare: $name"}
    $original=[Drawing.Bitmap]::FromFile($file.FullName)
    $w=$original.Width;$h=$original.Height
    if(!(IsPow2 $w) -or !(IsPow2 $h)){$original.Dispose();throw "$name is ${w}x${h}; textures must be powers of two"}
    $bitmap=[Drawing.Bitmap]::new($w,$h,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $graphics.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.DrawImageUnscaled($original,0,0)
    $graphics.Dispose();$original.Dispose()
    $bits=$bitmap.LockBits([Drawing.Rectangle]::new(0,0,$w,$h),[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels=[byte[]]::new($w*$h*4)
    [Runtime.InteropServices.Marshal]::Copy($bits.Scan0,$pixels,0,$pixels.Length)
    $bitmap.UnlockBits($bits);$bitmap.Dispose()
    $stream=[IO.MemoryStream]::new();$writer=[IO.BinaryWriter]::new($stream)
    # UI artwork retains its coarse pixel edges; handset and photo artwork stay bilinear.
    # Direct3D 9 native texture, clamp addressing, 8888 raster.
    $pixelArt = $name -match '^(app_|g_|w_|game_)' -or
        $name -in @('cam_shutter', 'disc', 'clock_face', 'glow') -or
        ($name.StartsWith('sm_') -and $name -notin @('sm_body', 'sm_back', 'sm_boot', 'sm_phone_normal', 'sm_phone_material'))
    $filter = if ($pixelArt -and -not $name.EndsWith('_64')) { [uint32]0x3301 } else { [uint32]0x3302 }
    $writer.Write([uint32]9);$writer.Write($filter)
    $label=[byte[]]::new(32);[Text.Encoding]::ASCII.GetBytes($name).CopyTo($label,0)
    $writer.Write($label);$writer.Write([byte[]]::new(32))
    $writer.Write([uint32]0x0500);$writer.Write([uint32]21)
    $writer.Write([uint16]$w);$writer.Write([uint16]$h)
    $writer.Write([byte[]]@(32,1,4,1));$writer.Write([uint32]$pixels.Length);$writer.Write($pixels)
    $native=(Chunk 1 $stream.ToArray())+(Chunk 3 ([byte[]]@()))
    $out.Write((Chunk 0x15 $native));$writer.Dispose()
}
$out.Write((Chunk 3 ([byte[]]@())))
[IO.File]::WriteAllBytes($Output,(Chunk 0x16 $dict.ToArray()));$out.Dispose()
Write-Output "Packed $($files.Count) textures: $Output"
