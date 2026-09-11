# ui-flow-test.ps1 - end-to-end test driving IN-APP code paths (ASCII only)
# Uses PostMessage WM_CONTEXTMENU (app-internal ListView_SetItemState) + posted
# WM_COMMAND copy while the context menu modal loop dispatches messages.
# Cross-process LVM_SETITEMSTATE is avoided: comctl32 marshaling crashes on this box.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public class W5 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string t);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);

    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr h, ref uint pid);

    [DllImport("user32.dll")]
    public static extern IntPtr GetOpenClipboardWindow();

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int l, t, r, b; }
}
'@

function Alive { [bool](Get-Process -Name kill-process-type -ErrorAction SilentlyContinue) }

$WM_CONTEXTMENU = 0x007B
$WM_COMMAND     = 0x0111
$WM_CANCELMODE  = 0x001F
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP   = 0x0202
$IDM_COPY       = 2101

Start-Process -FilePath (Join-Path $PSScriptRoot '..\build\kill-process-type.exe')
Start-Sleep -Seconds 3
$p = Get-Process -Name kill-process-type
$hMain = [IntPtr]$p.MainWindowHandle
$hList = [W5]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysListView32', $null)
$hTab  = [W5]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysTabControl32', $null)
Write-Host "list=$hList tab=$hTab alive=$(Alive)"

$cnt = [int][W5]::SendMessageW($hList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host "item count: $cnt"

# screen point over row 0 in the listview (below the header, ~50px from top)
$rc = New-Object W5+RECT
[void][W5]::GetWindowRect($hList, [ref]$rc)
$pt = ($rc.l + 200) -bor ((($rc.t + 50)) -shl 16)   # MAKELPARAM(x, y)
Write-Host "context point: x=$($rc.l + 200) y=$($rc.t + 50)"

# --- case 1: right-click row 0 -> context menu -> post copy command -> cancel menu ---
[void][W5]::PostMessageW($hList, $WM_CONTEXTMENU, $hList, [IntPtr]$pt)
Start-Sleep -Milliseconds 400
Write-Host "menu phase alive=$(Alive)"
[void][W5]::PostMessageW($hMain, $WM_COMMAND, [IntPtr]$IDM_COPY, [IntPtr]::Zero)
Start-Sleep -Milliseconds 500
[void][W5]::PostMessageW($hMain, $WM_CANCELMODE, [IntPtr]::Zero, [IntPtr]::Zero)
[void][W5]::PostMessageW($hMain, 0x0018, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_NULL
Start-Sleep -Milliseconds 500
Write-Host "after copy via context menu: alive=$(Alive)"

# close any modal dialog the app may have raised (e.g. "no path to copy")
function Close-AppDialogs {
    $p2 = Get-Process -Name kill-process-type -ErrorAction SilentlyContinue
    if (-not $p2) { return }
    $dlg = [IntPtr]::Zero
    for ($i = 0; $i -lt 30; $i++) {
        $dlg = [W5]::FindWindowExW([IntPtr]::Zero, $dlg, '#32770', $null)
        if ($dlg -eq [IntPtr]::Zero) { break }
        $wpid = 0
        [void][W5]::GetWindowThreadProcessId($dlg, [ref]$wpid)
        if ($wpid -eq $p2.Id) {
            Write-Host "modal dialog found (hwnd=$dlg), closing"
            [void][W5]::PostMessageW($dlg, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    }
}
Close-AppDialogs

# read clipboard from an independent native process (bypass PS OLE clipboard issues)
$clipread = Join-Path $PSScriptRoot '..\build\clipread.exe'
if (Test-Path $clipread) {
    $clip = (& cmd /c "chcp 65001 >nul & `"$clipread`"") -join "`n"
    Write-Host "clipboard: [$clip]"
} else {
    Write-Host "clipboard: (clipread.exe not built)"
}

# --- case 2: click tab 2 (Node/Python) via posted mouse messages ---
if ($hTab -ne [IntPtr]::Zero) {
    $cur = [int][W5]::SendMessageW($hTab, 0x1300 + 11, [IntPtr]::Zero, [IntPtr]::Zero)  # TCM_GETCURSEL
    Write-Host "current tab selection: $cur"
    $trc = New-Object W5+RECT
    [void][W5]::GetWindowRect($hTab, [ref]$trc)
    # WM_LBUTTONDOWN lParam uses CLIENT coords of the tab control
    $tp = 170 -bor (10 -shl 16)   # x=170 (2nd tab), y=10 (tab band)
    [void][W5]::PostMessageW($hTab, $WM_LBUTTONDOWN, [IntPtr]1, [IntPtr]$tp)
    [void][W5]::PostMessageW($hTab, $WM_LBUTTONUP, [IntPtr]0, [IntPtr]$tp)
    Start-Sleep -Milliseconds 800
    $cur2 = [int][W5]::SendMessageW($hTab, 0x1300 + 11, [IntPtr]::Zero, [IntPtr]::Zero)
    $cnt2 = [int][W5]::SendMessageW($hList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Host "after tab click: alive=$(Alive) tab now: $cur2 item count now: $cnt2 (was $cnt)"
}

# --- case 3: click column-0 header to sort (asc), click again (desc) ---
# row text read via MSAA (oleacc) - UIA only exposes a Pane in this environment
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class W7 {
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
}
'@
$hHdr = [W5]::FindWindowExW($hList, [IntPtr]::Zero, 'SysHeader32', $null)
if ($hHdr -ne [IntPtr]::Zero) {
    $guid = [guid]'618736e0-3c3d-11cf-810c-00aa00389b71'  # IID_IAccessible
    function New-AccObj {
        $o = $null
        [void][W7]::AccessibleObjectFromWindow($hList, [uint32]4294967292, [ref]$guid, [ref]$o)  # OBJID_CLIENT
        , $o
    }
    function Get-RowFirstTokens([int]$max) {
        # re-acquire the accessible object each call: stale after list rebuild
        $script:accObj = New-AccObj
        $toks = @()
        if ($null -eq $script:accObj) { return , $toks }
        $n = $script:accObj.accChildCount
        for ($i = 1; $i -le [Math]::Min($max, $n); $i++) {
            $name = $null
            try { $name = $script:accObj.accName($i) } catch { $name = $null }
            if ($name) { $toks += ($name -split '\s+')[0] }
        }
        , $toks
    }
    function Click-HdrCol0 {
        $pt = 40 -bor (10 -shl 16)   # header client coords: col 0, band
        [void][W5]::PostMessageW($hHdr, $WM_LBUTTONDOWN, [IntPtr]1, [IntPtr]$pt)
        [void][W5]::PostMessageW($hHdr, $WM_LBUTTONUP, [IntPtr]0, [IntPtr]$pt)
        Start-Sleep -Milliseconds 900
    }
    $before = Get-RowFirstTokens 8
    Click-HdrCol0
    $asc = Get-RowFirstTokens 8
    Click-HdrCol0
    $desc = Get-RowFirstTokens 8
    Write-Host "col0 before: $($before -join ',')"
    Write-Host "col0 asc   : $($asc -join ',')"
    Write-Host "col0 desc  : $($desc -join ',')"
    function Test-Mono([string[]]$t, [bool]$up) {
        if ($t.Count -lt 2) { return 'UNKNOWN (no rows read)' }
        $nums = @(); $allNum = $true
        foreach ($s in $t) { $v = 0; if ([int]::TryParse($s, [ref]$v)) { $nums += $v } else { $allNum = $false; break } }
        if (-not $allNum) { return 'n/a (non-numeric col)' }
        for ($i = 1; $i -lt $nums.Count; $i++) {
            if ($up -and $nums[$i] -lt $nums[$i - 1]) { return 'FAIL' }
            if (-not $up -and $nums[$i] -gt $nums[$i - 1]) { return 'FAIL' }
        }
        return 'PASS'
    }
    Write-Host "sort asc check : $(Test-Mono $asc $true)"
    Write-Host "sort desc check: $(Test-Mono $desc $false)"
    Write-Host "alive after sort test: $(Alive)"
}

# --- who holds the clipboard open? ---
$cbw = [W5]::GetOpenClipboardWindow()
if ($cbw -ne [IntPtr]::Zero) {
    $cpid = 0
    [void][W5]::GetWindowThreadProcessId($cbw, [ref]$cpid)
    $cproc = Get-Process -Id $cpid -ErrorAction SilentlyContinue
    Write-Host "clipboard holder: pid=$cpid name=$($cproc.ProcessName)"
} else {
    Write-Host "clipboard holder: none (closed at check time)"
}

Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'done'
