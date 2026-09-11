# make-icon.ps1 - 生成 32x32 托盘/应用图标（PNG 内嵌式 ICO，Vista+ 支持，带 Alpha）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File res\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $dir 'app.ico'
$tmpPng = Join-Path $env:TEMP 'kpt-icon.png'

$bmp = New-Object System.Drawing.Bitmap -ArgumentList @(32, 32)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)

# 深灰圆底
$bgColor = [System.Drawing.Color]::FromArgb(255, 40, 44, 52)
$bg = New-Object System.Drawing.SolidBrush -ArgumentList @($bgColor)
$g.FillEllipse($bg, 1, 1, 30, 30)

# 红色圆头 X
$xColor = [System.Drawing.Color]::FromArgb(255, 240, 84, 79)
$pen = New-Object System.Drawing.Pen -ArgumentList @($xColor, [single]5)
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawLine($pen, 10, 10, 22, 22)
$g.DrawLine($pen, 22, 10, 10, 22)
$g.Dispose()

$bmp.Save($tmpPng, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

$png = [System.IO.File]::ReadAllBytes($tmpPng)
Remove-Item -LiteralPath $tmpPng -ErrorAction SilentlyContinue

# 手工拼 ICO 容器：6 字节头 + 16 字节目录项 + PNG 数据
$header = [byte[]](0, 0, 1, 0, 1, 0)
$dir = New-Object byte[] -ArgumentList @(16)
$dir[0] = 32                                  # 宽
$dir[1] = 32                                  # 高
$dir[4] = 0;  $dir[5] = 1                     # 色彩平面 = 1
$dir[6] = 0;  $dir[7] = 32                    # 位深 = 32
$sizeB = [BitConverter]::GetBytes([uint32]$png.Length)
$dir[8] = $sizeB[0]; $dir[9] = $sizeB[1]; $dir[10] = $sizeB[2]; $dir[11] = $sizeB[3]
$dir[12] = 22;                                 # 数据偏移 = 6 + 16

$ico = $header + $dir + $png
[System.IO.File]::WriteAllBytes($out, $ico)
Write-Host "icon written: $out ($($ico.Length) bytes)"
