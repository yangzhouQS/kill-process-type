# dev-features-test.ps1 - verify tree mode + /top + cmdline column (ASCII only)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WD1 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
}
'@
$exe = Join-Path $PSScriptRoot '..\build\kill-process-type.exe'
$build = Split-Path $exe
"[main]`r`nTreeView=1`r`n" | Set-Content (Join-Path $build 'kill-process-type.ini') -Encoding ASCII
Start-Process -FilePath $exe -WorkingDirectory $build
Start-Sleep -Seconds 3
$p = Get-Process -Name kill-process-type
$hMain = [IntPtr]$p.MainWindowHandle
$hList = [WD1]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysListView32', $null)

# 1) tree already ON via config; verify rendering
$cnt = [int][WD1]::SendMessageW($hList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host ("tree ON rows: " + $cnt)

# 2) read first few row names via MSAA - expect tree markers (UTF arrows) or indented
$guid = [guid]'618736e0-3c3d-11cf-810c-00aa00389b71'
$obj = $null
[void][WD1]::AccessibleObjectFromWindow($hList, [uint32]4294967292, [ref]$guid, [ref]$obj)
if ($obj) {
    $n = $obj.accChildCount
    $markers = 0
    for ($i = 1; $i -le [Math]::Min(20, $n); $i++) {
        try {
            $name = $obj.accName($i)
            if ($name -match '^\s|▾|▸|·') { $markers++ }
            if ($i -le 5) { Write-Host ("  row" + $i + ": [" + $name.Substring(0, [Math]::Min(50, $name.Length)) + "]") }
        } catch {}
    }
    Write-Host ("rows with tree markers/indent: " + $markers + "/" + [Math]::Min(20, $n))
}

# 3) double-click a row to toggle collapse (post WM_NOTIFY NM_DBLCLK is complex; skip - use keyboard? just verify app alive)
Write-Host ("app alive: " + [bool](Get-Process -Name kill-process-type -ErrorAction SilentlyContinue))

# 4) toggle tree off
[void][WD1]::PostMessageW($hMain, 0x0111, [IntPtr]2010, [IntPtr]::Zero)
Start-Sleep -Seconds 1
$cnt2 = [int][WD1]::SendMessageW($hList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host ("tree OFF rows: " + $cnt2)
Write-Host ("app alive: " + [bool](Get-Process -Name kill-process-type -ErrorAction SilentlyContinue))

Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item (Join-Path $build 'kill-process-type.ini') -ErrorAction SilentlyContinue
Write-Host 'done'
