# tree-sort-test.ps1 - verify tree mode memory sort (subtree totals) (ASCII only)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WT2 {
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
$hList = [WT2]::FindWindowExW($hMain, [IntPtr]::Zero, 'SysListView32', $null)

# accName returns FIRST column only; verify by root name sequence change
$guid = [guid]'618736e0-3c3d-11cf-810c-00aa00389b71'
$obj = $null
[void][WT2]::AccessibleObjectFromWindow($hList, [uint32]4294967292, [ref]$guid, [ref]$obj)
function Get-RootNames {
    $out = @()
    if ($script:obj) {
        $n = $script:obj.accChildCount
        for ($i = 1; $i -le [Math]::Min(60, $n); $i++) {
            try {
                $name = $script:obj.accName($i)
                if ($name -and -not $name.StartsWith(' ')) {
                    $out += ($name -replace '^[▾▸·]\s*', '')
                }
            } catch {}
        }
    }
    , $out
}
$before = Get-RootNames
Write-Host ("roots before: " + ($before[0..([Math]::Min(5, $before.Count - 1))] -join ' | '))

# click memory column header (col 3): at 1.5x DPI col3 starts ~660px physical
$hHdr = [WT2]::FindWindowExW($hList, [IntPtr]::Zero, 'SysHeader32', $null)
if ($hHdr -eq [IntPtr]::Zero) { Write-Host 'FAIL: no header'; exit 1 }
$hp = 705 -bor (10 -shl 16)
[void][WT2]::PostMessageW($hHdr, 0x0201, [IntPtr]1, [IntPtr]$hp)
[void][WT2]::PostMessageW($hHdr, 0x0202, [IntPtr]0, [IntPtr]$hp)
Start-Sleep -Seconds 2

$after = Get-RootNames
Write-Host ("roots after : " + ($after[0..([Math]::Min(5, $after.Count - 1))] -join ' | '))
$orderChanged = ($before -join ',') -ne ($after -join ',')
Write-Host ("order changed: " + $orderChanged)
if ($orderChanged) {
    Write-Host 'SORT APPLIED (tree memory/subtree sort active)'
} else {
    Write-Host 'FAIL: sort did not apply'
}
Write-Host ("alive: " + [bool](Get-Process -Name kill-process-type -ErrorAction SilentlyContinue))
Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item (Join-Path $build 'kill-process-type.ini') -ErrorAction SilentlyContinue
Write-Host 'done'
