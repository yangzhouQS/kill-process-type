# wp-e2e-test.ps1 - E2E test for WP2/WP3/WP5 buttons (ASCII only)
# Verifies buttons exist, trigger correctly, and status updates (no full kilo wait)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WE1 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
}
'@
$exe = Join-Path $PSScriptRoot '..\build\kill-process-type.exe'
$build = Split-Path $exe
Start-Process -FilePath $exe -WorkingDirectory $build
Start-Sleep -Seconds 3
$p = Get-Process -Name kill-process-type
$hMain = [IntPtr]$p.MainWindowHandle

# Find all buttons by text
function Find-Btn([string]$text) {
    $btn = [WE1]::FindWindowExW($hMain, [IntPtr]::Zero, 'Button', $null)
    while ($btn -ne [IntPtr]::Zero) {
        $sb = New-Object System.Text.StringBuilder 128
        [void][WE1]::GetWindowTextW($btn, $sb, 128)
        if ($sb.ToString() -eq $text) { return $btn }
        $btn = [WE1]::FindWindowExW($hMain, $btn, 'Button', $null)
    }
    return [IntPtr]::Zero
}

$btnBatch = Find-Btn 'AI 风险扫描'
$btnLog = Find-Btn 'AI 复盘日志'
Write-Host ("batch button found: " + ($btnBatch -ne [IntPtr]::Zero) + " visible: " + [WE1]::IsWindowVisible($btnBatch))
Write-Host ("log button found: " + ($btnLog -ne [IntPtr]::Zero) + " visible: " + [WE1]::IsWindowVisible($btnLog))

# Tab 4 (AI Diagnosis)
$hTab = [WE1]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysTabControl32', $null)
$tp = 460 -bor (10 -shl 16)
[void][WE1]::PostMessageW($hTab, 0x0201, [IntPtr]1, [IntPtr]$tp)
[void][WE1]::PostMessageW($hTab, 0x0202, [IntPtr]0, [IntPtr]$tp)
Start-Sleep -Seconds 2
$sel = [WE1]::SendMessageW($hTab, 0x1300 + 11, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
Write-Host ("diag tab sel: " + $sel)

# Check diag button visible
$btnDiag = Find-Btn '生成全局诊断快照'
Write-Host ("diag button found: " + ($btnDiag -ne [IntPtr]::Zero) + " visible: " + [WE1]::IsWindowVisible($btnDiag))

# Click diag button to open the window
if ($btnDiag -ne [IntPtr]::Zero) {
    [void][WE1]::SendMessageW($btnDiag, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) # BM_CLICK
    Start-Sleep -Seconds 2
    # Check if diag window opened
    $hDiag = [IntPtr]::Zero
    # Enumerate for KptDiagDlg class
    Add-Type -Namespace W -Name E -MemberDefinition '[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l); public delegate bool EnumProc(IntPtr h, IntPtr l); [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int max); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, ref uint pid);'
    $found = $false
    $cb = { param($h, $l)
        $wp = 0; [void][W.E]::GetWindowThreadProcessId($h, [ref]$wp)
        if ($wp -eq $script:ppId) {
            $cn = New-Object System.Text.StringBuilder 256
            [void][W.E]::GetClassNameW($h, $cn, 256)
            if ($cn.ToString() -eq 'KptDiagDlg') { $script:hit = $h; $script:found = $true; return $false }
        }
        return $true
    }
    $script:ppId = $p.Id; $script:hit = [IntPtr]::Zero; $script:found = $false
    [void][W.E]::EnumWindows($cb, [IntPtr]::Zero)
    Write-Host ("diag window opened: " + $script:found)
    if ($script:found) {
        # Check status text
        $sb = New-Object System.Text.StringBuilder 256
        $hStatus = [WE1]::FindWindowExW($script:hit, [IntPtr]::Zero, 'Static', $null)
        if ($hStatus -ne [IntPtr]::Zero) {
            [void][WE1]::GetWindowTextW($hStatus, $sb, 256)
            Write-Host ("  status: " + $sb.ToString())
        }
        # Close diag window
        [void][WE1]::PostMessageW($script:hit, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 500
    }
}

# Go back to ALL tab, verify batch button visible
$tp0 = 60 -bor (10 -shl 16)
[void][WE1]::PostMessageW($hTab, 0x0201, [IntPtr]1, [IntPtr]$tp0)
[void][WE1]::PostMessageW($hTab, 0x0202, [IntPtr]0, [IntPtr]$tp0)
Start-Sleep -Seconds 2
$sel0 = [WE1]::SendMessageW($hTab, 0x1300 + 11, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
$batchVis = [WE1]::IsWindowVisible($btnBatch)
Write-Host ("back to tab0 sel=" + $sel0 + " batch visible=" + $batchVis)

Write-Host ("alive: " + [bool](Get-Process -Name kill-process-type -ErrorAction SilentlyContinue))
Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item (Join-Path $build 'kill-process-type.ini') -ErrorAction SilentlyContinue
Write-Host 'done'
