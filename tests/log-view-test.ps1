# log-view-test.ps1 - verify kill log + log tab (ASCII only)
# 1) CLI kill a dummy -> log line written
# 2) GUI: switch to log tab (index 3), verify >=1 row via MSAA
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WL1 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string t);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
}
'@
$exe = Join-Path $PSScriptRoot '..\build\kill-process-type.exe'
$build = Split-Path $exe

# 1) CLI kill -> log
$victim = Start-Process "$env:WINDIR\System32\cmd.exe" -ArgumentList '/k','title log-view-victim' -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
cmd /c "`"$exe`" /kill $($victim.Id)" | Out-Null
Start-Sleep -Milliseconds 800
$dead = -not (Get-Process -Id $victim.Id -ErrorAction SilentlyContinue)
$logFile = Join-Path $build 'kill-process-type.log'
$hasLine = (Test-Path $logFile) -and ((Get-Content $logFile -Encoding UTF8 | Select-String $victim.Id.ToString()).Count -gt 0)
Write-Host ("CLI kill: victim-dead=" + $dead + " log-line=" + $hasLine)

# 2) GUI log tab
Start-Process -FilePath $exe -WorkingDirectory $build
Start-Sleep -Seconds 3
$p = Get-Process -Name kill-process-type
$hMain = [IntPtr]$p.MainWindowHandle
$hTab = [WL1]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysTabControl32', $null)
# click tab index 3 (log): scan positions (window DPI/position may vary)
$cur = -1
foreach ($x in 260,310,360,420,480,200,150) {
    $tp = $x -bor (10 -shl 16)
    [void][WL1]::PostMessageW($hTab, 0x0201, [IntPtr]1, [IntPtr]$tp)
    [void][WL1]::PostMessageW($hTab, 0x0202, [IntPtr]0, [IntPtr]$tp)
    Start-Sleep -Milliseconds 800
    $cur = [int][WL1]::SendMessageW($hTab, 0x1300 + 11, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($cur -eq 3) { break }
}
$cur = [int][WL1]::SendMessageW($hTab, 0x1300 + 11, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host ("tab selection: " + $cur + " (expect 3)")

$hList = [WL1]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysListView32', $null)
$cnt = [int][WL1]::SendMessageW($hList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host ("log rows: " + $cnt + " (expect >=1)")

$guid = [guid]'618736e0-3c3d-11cf-810c-00aa00389b71'
$obj = $null
[void][WL1]::AccessibleObjectFromWindow($hList, [uint32]4294967292, [ref]$guid, [ref]$obj)
if ($obj) {
    $n = $obj.accChildCount
    for ($i = 1; $i -le [Math]::Min(3, $n); $i++) {
        try { Write-Host ("  row" + $i + ": " + $obj.accName($i)) } catch {}
    }
} else { Write-Host '  (MSAA unavailable)' }

Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'done'
