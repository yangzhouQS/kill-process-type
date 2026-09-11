# ai-e2e-test.ps1 - end-to-end AI risk analysis via in-app path (ASCII only)
# 1) launch GUI  2) post WM_CONTEXTMENU over row 0 (menu opens, sets AI row index)
# 3) post WM_COMMAND(IDM_LIST_AI_ANALYZE) -> kilo headless run (~40-90s)
# 4) read the AI dialog EDIT text via WM_GETTEXT  5) verify + cleanup
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W8 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string t);

    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageWText(IntPtr h, uint m, IntPtr w, System.Text.StringBuilder l);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int max);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr h, ref uint pid);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);

    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int l, t, r, b; }
}
'@

$exe = Join-Path $PSScriptRoot '..\build\kill-process-type.exe'
Start-Process -FilePath $exe
Start-Sleep -Seconds 3
$p = Get-Process -Name kill-process-type
$hMain = [IntPtr]$p.MainWindowHandle
$hList = [W8]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysListView32', $null)
Write-Host "app ready: main=$hMain list=$hList"

# open context menu over a real process row (~row 2, below header)
# NOTE: row 0 in ALL view is [System Process] (PID 0) which has no AI menu item
$rc = New-Object W8+RECT
[void][W8]::GetWindowRect($hList, [ref]$rc)
$pt = ($rc.l + 200) -bor ((($rc.t + 100)) -shl 16)
[void][W8]::PostMessageW($hList, 0x007B, $hList, [IntPtr]$pt)   # WM_CONTEXTMENU
Start-Sleep -Milliseconds 600

# trigger AI analysis while menu loop dispatches; then cancel menu
[void][W8]::PostMessageW($hMain, 0x0111, [IntPtr]2104, [IntPtr]::Zero)  # IDM_LIST_AI_ANALYZE
Start-Sleep -Milliseconds 400
[void][W8]::PostMessageW($hMain, 0x001F, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CANCELMODE

# AI dialog should exist (class KptAiRiskDlg); allow time for dispatch
# NOTE: FindWindow by custom class fails in this host - use EnumWindows instead
Start-Sleep -Seconds 2
function Find-WindowByClassInPid([string]$cls, [int]$procId) {
    $hit = [IntPtr]::Zero
    $cb = {
        param($h, $l)
        $wp = 0
        [void][W8]::GetWindowThreadProcessId($h, [ref]$wp)
        if ($wp -eq $script:wantPid) {
            $cn = New-Object System.Text.StringBuilder 256
            [void][W8]::GetClassNameW($h, $cn, 256)
            if ($cn.ToString() -eq $script:wantCls) { $script:hit = $h; return $false }
        }
        return $true
    }
    $script:wantPid = $procId
    $script:wantCls = $cls
    $script:hit = [IntPtr]::Zero
    [void][W8]::EnumWindows($cb, [IntPtr]::Zero)
    , $script:hit
}
$appProc = Get-Process -Name kill-process-type -ErrorAction SilentlyContinue
$dlg = [IntPtr]::Zero
if ($appProc) {
    for ($i = 0; $i -lt 10; $i++) {
        $dlg = Find-WindowByClassInPid 'KptAiRiskDlg' $appProc.Id
        if ($dlg -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 500
    }
}
# also locate the content child via EnumChildWindows (FindWindow flaky in this host)
# RichEdit class is RICHEDIT50W / RichEdit20W; fallback plain Edit; plus the Static status
$edit = [IntPtr]::Zero
$statusWnd = [IntPtr]::Zero
if ($dlg -ne [IntPtr]::Zero) {
    $cb2 = {
        param($h, $l)
        $cn = New-Object System.Text.StringBuilder 256
        [void][W8]::GetClassNameW($h, $cn, 256)
        $cls = $cn.ToString()
        if ($script:editHit -eq [IntPtr]::Zero -and $cls -match 'RichEdit|^Edit$') { $script:editHit = $h }
        if ($script:statHit -eq [IntPtr]::Zero -and $cls -eq 'Static') { $script:statHit = $h }
        return $true
    }
    $script:editHit = [IntPtr]::Zero
    $script:statHit = [IntPtr]::Zero
    [void][W8]::EnumChildWindows($dlg, $cb2, [IntPtr]::Zero)
    $edit = $script:editHit
    $statusWnd = $script:statHit
}
if ($dlg -eq [IntPtr]::Zero) {
    Write-Host 'FAIL: AI dialog not found'
    Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}
Write-Host "AI dialog: $dlg"

# wait for kilo (up to 300s incl. one auto-retry), poll the EDIT control
Write-Host "edit control: $edit (polling up to 300s for kilo...)"
$final = ''
for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Seconds 3
    if (-not (Get-Process -Name kill-process-type -ErrorAction SilentlyContinue)) {
        Write-Host 'FAIL: app died during analysis'
        exit 1
    }
    $sb = New-Object System.Text.StringBuilder 16384
    [void][W8]::SendMessageWText($edit, 0x000D, [IntPtr]16384, $sb)  # WM_GETTEXT
    $txt = $sb.ToString()
    if ($txt -and $txt -notmatch 'kilo' -and $txt -match '风险|评级|进程') {
        $final = $txt
        break
    }
    if ($i % 10 -eq 4 -and $statusWnd -ne [IntPtr]::Zero) {
        $sb2 = New-Object System.Text.StringBuilder 256
        [void][W8]::SendMessageWText($statusWnd, 0x000D, [IntPtr]256, $sb2)
        Write-Host ("  [" + (3 * ($i + 1)) + "s] status: " + $sb2.ToString())
    }
    if ($i % 10 -eq 9) { Write-Host ("  still waiting... " + (3 * ($i + 1)) + "s, edit='" + ($txt -replace "`r`n", ' ').Substring(0, [Math]::Min(60, $txt.Length)) + "'") }
}
if (-not $final) {
    $sb = New-Object System.Text.StringBuilder 16384
    [void][W8]::SendMessageWText($edit, 0x000D, [IntPtr]16384, $sb)
    $final = $sb.ToString()
}
Write-Host '--- AI analysis result ---'
Write-Host $final
Write-Host '--------------------------'
Write-Host ("app alive: " + [bool](Get-Process -Name kill-process-type -ErrorAction SilentlyContinue))
Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'done'
