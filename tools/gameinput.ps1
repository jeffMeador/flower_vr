# Drives the running Flower window with real SendInput events (scan codes, so
# DirectInput/raw-input games see them), and converts frame dumps for viewing.
# Usage: . .\gameinput.ps1 ; Focus-Game ; Send-Key 0x1C ; Click-At 0.5 0.5
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class GI {
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] public struct U { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public U u; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  public static void Key(ushort scan, bool up) {
    var i = new INPUT[1]; i[0].type = 1; i[0].u.ki.wScan = scan;
    i[0].u.ki.dwFlags = 0x0008u | (up ? 0x0002u : 0u); // SCANCODE | KEYUP
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void VKey(ushort vk, bool up) {
    var i = new INPUT[1]; i[0].type = 1; i[0].u.ki.wVk = vk; i[0].u.ki.dwFlags = up ? 0x0002u : 0u;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Mouse(uint flags, int dx, int dy) {
    var i = new INPUT[1]; i[0].type = 0; i[0].u.mi.dwFlags = flags; i[0].u.mi.dx = dx; i[0].u.mi.dy = dy;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
}
"@

$script:GameDir = "$env:USERPROFILE\Desktop\Flower_GOG"

function Focus-Game {
  $p = Get-Process Flower -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { throw 'Flower is not running' }
  [GI]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
  [GI]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
  Start-Sleep -Milliseconds 300
}

# Scan codes: Enter 0x1C, Esc 0x01, Space 0x39, W 0x11, A 0x1E, S 0x1F, D 0x20
function Send-Key([int]$scan, [int]$holdMs = 80) {
  [GI]::Key([uint16]$scan, $false); Start-Sleep -Milliseconds $holdMs; [GI]::Key([uint16]$scan, $true)
}
function Send-VKey([int]$vk, [int]$holdMs = 80) {
  [GI]::VKey([uint16]$vk, $false); Start-Sleep -Milliseconds $holdMs; [GI]::VKey([uint16]$vk, $true)
}

# fx, fy: fraction of the primary screen (0..1)
function Click-At([double]$fx, [double]$fy, [int]$holdMs = 100) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  [GI]::SetCursorPos([int]($b.X + $fx * $b.Width), [int]($b.Y + $fy * $b.Height)) | Out-Null
  Start-Sleep -Milliseconds 50
  [GI]::Mouse(0x0002, 0, 0); Start-Sleep -Milliseconds $holdMs; [GI]::Mouse(0x0004, 0, 0)
}
function Hold-Mouse([int]$ms) { [GI]::Mouse(0x0002, 0, 0); Start-Sleep -Milliseconds $ms; [GI]::Mouse(0x0004, 0, 0) }
function Move-Mouse([int]$dx, [int]$dy) { [GI]::Mouse(0x0001, $dx, $dy) }

# Downscale a frame dump (latest by default) to a PNG; returns its path.
function Show-Frame([string]$name, [int]$width = 1536, [string]$out = "$env:TEMP\flower_view.png") {
  $src = if ($name) { Get-Item (Join-Path $script:GameDir $name) } else { Get-ChildItem "$script:GameDir\frame_*.bmp" | Where-Object { $_.LastWriteTime -lt (Get-Date).AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1 }
  $img = [System.Drawing.Image]::FromFile($src.FullName)
  $h = [int]($img.Height * $width / $img.Width)
  $bmp = New-Object System.Drawing.Bitmap $width, $h
  $gr = [System.Drawing.Graphics]::FromImage($bmp); $gr.DrawImage($img, 0, 0, $width, $h)
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $gr.Dispose(); $img.Dispose(); $bmp.Dispose()
  "$($src.Name) -> $out"
}
Add-Type -AssemblyName System.Windows.Forms

# Virtual controller (needs vrmod.ini [debug] fakepad=1). e.g. Set-Pad -0.8 0 A
function Set-Pad([double]$lx = 0, [double]$ly = 0, [string[]]$buttons = @()) {
  $line = ('{0:F3} {1:F3} {2}' -f $lx, $ly, ($buttons -join ' ')).Trim()
  [IO.File]::WriteAllText("$script:GameDir\vrmod_pad.txt", $line)
}

# Desktop screenshot (works with this game's borderless fullscreen), downscaled.
function Snap([string]$out = "$env:TEMP\scr.png", [int]$width = 1536) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $full = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($full); $g.CopyFromScreen($b.X, $b.Y, 0, 0, $full.Size)
  $h = [int]($b.Height * $width / $b.Width)
  $s = New-Object System.Drawing.Bitmap $width, $h
  $g2 = [System.Drawing.Graphics]::FromImage($s); $g2.DrawImage($full, 0, 0, $width, $h)
  $s.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $g2.Dispose(); $full.Dispose(); $s.Dispose()
}

# Stack several PNGs vertically into one image for quick review.
function Stack-Images([string[]]$paths, [string]$out = "$env:TEMP\strip.png", [int]$width = 768) {
  $imgs = $paths | ForEach-Object { [System.Drawing.Image]::FromFile($_) }
  $h = [int]($imgs[0].Height * $width / $imgs[0].Width)
  $bmp = New-Object System.Drawing.Bitmap $width, ($h * $imgs.Count)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  for ($i = 0; $i -lt $imgs.Count; $i++) { $g.DrawImage($imgs[$i], 0, $i * $h, $width, $h) }
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose(); $imgs | ForEach-Object Dispose
}

# Launch the GOG build (unless running) and drive the menu into level 1 with
# the virtual pad. Retries: menu timing varies. Success = the mod logs a
# perspective camera ("proj from VP"), which only happens in a level.
function Start-Level1([switch]$Restart) {
  $log = "$script:GameDir\vrmod.log"
  if ($Restart -or -not (Get-Process Flower -ErrorAction SilentlyContinue)) {
    Get-Process Flower -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    Set-Pad 0 0
    Start-Process -FilePath "$script:GameDir\Flower.exe" -WorkingDirectory $script:GameDir
    Start-Sleep 15
    Set-Pad 0 0 A; Start-Sleep -Milliseconds 400; Set-Pad 0 0   # past the title card
    Start-Sleep 12
  }
  for ($try = 1; $try -le 6; $try++) {
    if (Select-String -Path $log -Pattern 'proj from VP' -Quiet) { break }
    # The pot is left of center: steer to it, then hold the trigger right away,
    # before the camera drifts back out (it does within ~2s).
    Set-Pad -0.7 0; Start-Sleep -Milliseconds 1200
    Set-Pad 0 0 RT; Start-Sleep 5; Set-Pad 0 0
    for ($w = 0; $w -lt 12; $w++) {
      Start-Sleep 5
      if (Select-String -Path $log -Pattern 'proj from VP' -Quiet) { break }
    }
  }
  $ok = Select-String -Path $log -Pattern 'proj from VP' -Quiet
  Start-Sleep 10   # finish fade-in
  Focus-Game
  "in level: $ok"
}
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class Stereo {
  static byte[] Px(Bitmap b, out int stride) {
    var d = b.LockBits(new Rectangle(0,0,b.Width,b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
    stride = d.Stride; var a = new byte[stride*b.Height]; Marshal.Copy(d.Scan0, a, 0, a.Length); b.UnlockBits(d); return a;
  }
  // Red from left, green/blue from right. Also returns mean abs difference (0..255).
  public static double Anaglyph(Bitmap l, Bitmap r, string outPath) {
    int s1, s2; var a = Px(l, out s1); var b = Px(r, out s2);
    var o = new Bitmap(l.Width, l.Height, PixelFormat.Format24bppRgb);
    var d = o.LockBits(new Rectangle(0,0,o.Width,o.Height), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
    var c = new byte[d.Stride*o.Height]; double diff = 0;
    for (int y=0;y<o.Height;y++) for (int x=0;x<o.Width;x++) {
      int i=y*s1+x*3; // BGR
      double gl = 0.3*a[i+2]+0.59*a[i+1]+0.11*a[i], gr = 0.3*b[i+2]+0.59*b[i+1]+0.11*b[i];
      c[i+2]=(byte)gl; c[i+1]=(byte)gr; c[i]=(byte)gr; diff += Math.Abs(gl-gr);
    }
    double n = (double)o.Width*o.Height;
    Marshal.Copy(c, 0, d.Scan0, c.Length); o.UnlockBits(d); o.Save(outPath, ImageFormat.Png); o.Dispose();
    return diff/n;
  }
}
"@

function Load-Scaled([string]$path, [int]$width) {
  $img = [System.Drawing.Image]::FromFile($path)
  $h = [int]($img.Height * $width / $img.Width)
  $bmp = New-Object System.Drawing.Bitmap $width, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.DrawImage($img, 0, 0, $width, $h); $g.Dispose(); $img.Dispose()
  $bmp
}

# Press F12 in-game (dumps two consecutive frames = both eyes), build an anaglyph.
function Capture-StereoPair([int]$width = 1920, [string]$out = "$env:TEMP\anaglyph.png") {
  $before = Get-ChildItem "$script:GameDir\frame_*.bmp" -ErrorAction SilentlyContinue | ForEach-Object Name
  Focus-Game; Send-VKey 0x7B 60
  Start-Sleep 6
  $new = Get-ChildItem "$script:GameDir\frame_*.bmp" | Where-Object { $before -notcontains $_.Name } |
    Sort-Object { [int]($_.BaseName -replace 'frame_','') }
  if ($new.Count -lt 2) { throw "expected 2 new dumps, got $($new.Count)" }
  $l = Load-Scaled $new[0].FullName $width; $r = Load-Scaled $new[1].FullName $width
  $diff = [Stereo]::Anaglyph($l, $r, $out); $l.Dispose(); $r.Dispose()
  "$($new[0].Name) + $($new[1].Name) -> $out  meanAbsDiff=$([math]::Round($diff,2))"
}
