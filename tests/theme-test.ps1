# theme-test.ps1 - verify config/theme/settings port (ASCII only)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WT {
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
$ini = Join-Path $PSScriptRoot '..\build\kill-process-type.ini'

# explicit light theme (this machine's system default is dark; auto would follow it)
"[main]`r`nTheme=1`r`n" | Set-Content $ini -Encoding ASCII
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
Start-Sleep -Seconds 3
$p = Get-Process -Name kill-process-type
$hMain = [IntPtr]$p.MainWindowHandle
$hList = [WT]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysListView32', $null)
# textcolor via LVM_GETTEXTCOLOR 0x1023 (GETBKCOLOR unreliable with DOUBLEBUFFER)
$bkLight = [int][WT]::SendMessageW($hList, 0x1023, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host "light theme: textcolor=0x$($bkLight.ToString('X')) (expect system dark-text, NOT EBEBEB)"

# open settings window via tray command id
[void][WT]::PostMessageW($hMain, 0x0111, [IntPtr]1107, [IntPtr]::Zero)  # IDM_TRAY_SETTINGS
Start-Sleep -Seconds 2
$pp = Get-Process -Name kill-process-type
$found = $false
$cb = {
    param($h, $l)
    $wp = 0
    [void][WT]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($wp -eq $script:ppId) {
        $cn = New-Object System.Text.StringBuilder 256
        [void][WT]::GetClassNameW($h, $cn, 256)
        if ($cn.ToString() -eq 'KptSettingsDlg') { $script:hit = $h; return $false }
    }
    return $true
}
$script:ppId = $pp.Id
$script:hit = [IntPtr]::Zero
[void][WT]::EnumWindows($cb, [IntPtr]::Zero)
Write-Host "settings window: found=$($script:hit -ne [IntPtr]::Zero) hwnd=$($script:hit)"
# close settings
if ($script:hit -ne [IntPtr]::Zero) {
    [void][WT]::PostMessageW($script:hit, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
}
Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

# write dark theme into config, relaunch, verify listview dark background
"[main]`r`nTheme=2`r`n" | Set-Content $ini -Encoding ASCII
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
Start-Sleep -Seconds 3
$p2 = Get-Process -Name kill-process-type
$hMain2 = [IntPtr]$p2.MainWindowHandle
$hList2 = [WT]::FindWindowExW($hMain2, [IntPtr]::Zero, 'SysListView32', $null)
$bkDark = [int][WT]::SendMessageW($hList2, 0x1023, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host "dark theme : textcolor=0x$($bkDark.ToString('X')) (expect EBEBEB)"
Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force

# config persisted by both runs?
Write-Host "config file exists: $(Test-Path $ini)"
Remove-Item $ini -ErrorAction SilentlyContinue
Write-Host 'done'
