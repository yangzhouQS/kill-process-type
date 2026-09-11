# theme-live-test.ps1 - live theme switch fan-out via settings radios (ASCII only)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WTL {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string t);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr h, ref uint pid);
}
'@
$exe = Join-Path $PSScriptRoot '..\build\kill-process-type.exe'
$ini = Join-Path (Split-Path $exe) 'kill-process-type.ini'
"[main]`r`nTheme=2`r`n" | Set-Content $ini -Encoding ASCII   # start dark

Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
Start-Sleep -Seconds 3
$p = Get-Process -Name kill-process-type
$hMain = [IntPtr]$p.MainWindowHandle
$hList = [WTL]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysListView32', $null)

function Get-TextColor([IntPtr]$lv) { [int][WTL]::SendMessageW($lv, 0x1023, [IntPtr]::Zero, [IntPtr]::Zero) }
Write-Host ("initial (dark): textcolor=0x" + (Get-TextColor $hList).ToString('X') + " expect EBEBEB")

# open settings
[void][WTL]::PostMessageW($hMain, 0x0111, [IntPtr]1107, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$cb = {
    param($h, $l)
    $wp = 0
    [void][WTL]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($wp -eq $script:ppId) {
        $cn = New-Object System.Text.StringBuilder 256
        [void][WTL]::GetClassNameW($h, $cn, 256)
        if ($cn.ToString() -eq 'KptSettingsDlg') { $script:hit = $h; return $false }
    }
    return $true
}
$script:ppId = $p.Id
$script:hit = [IntPtr]::Zero
[void][WTL]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:hit -eq [IntPtr]::Zero) { Write-Host 'FAIL: settings not found'; exit 1 }
$hSet = $script:hit
Write-Host "settings: $hSet"

# click LIGHT radio (IDC_SET_THEME_LIGHT=2304, BN_CLICKED=0)
[void][WTL]::PostMessageW($hSet, 0x0111, [IntPtr]2304, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$c1 = Get-TextColor $hList
Write-Host ("after LIGHT radio: textcolor=0x" + $c1.ToString('X') + " expect 0 (light, live fan-out)")

# click DARK radio (2305)
[void][WTL]::PostMessageW($hSet, 0x0111, [IntPtr]2305, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$c2 = Get-TextColor $hList
Write-Host ("after DARK radio : textcolor=0x" + $c2.ToString('X') + " expect EBEBEB")

# click AUTO radio (2303): system is dark -> expect EBEBEB
[void][WTL]::PostMessageW($hSet, 0x0111, [IntPtr]2303, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$c3 = Get-TextColor $hList
Write-Host ("after AUTO radio : textcolor=0x" + $c3.ToString('X') + " expect EBEBEB (system=dark)")

$ok = ((Get-TextColor $hList) -eq 0xEBEBEB) -and ($c1 -eq 0)
Write-Host ("LIVE SWITCH " + $(if ($ok) { 'PASS' } else { 'FAIL' }))
[void][WTL]::PostMessageW($hSet, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $ini -ErrorAction SilentlyContinue
Write-Host 'done'
