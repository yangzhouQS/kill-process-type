# cli-test.ps1 - headless CLI mode verification (ASCII only)
$exe = Join-Path $PSScriptRoot '..\build\kill-process-type.exe'

Write-Host '=== /list ==='
$listJson = cmd /c "`"$exe`" /list"
$list = $listJson -join "`n" | ConvertFrom-Json
Write-Host ("count: " + $list.count + "  first: " + $list.processes[0].name + " pid=" + $list.processes[0].pid + " type=" + $list.processes[0].type)

Write-Host '=== /ports ==='
$portsJson = cmd /c "`"$exe`" /ports"
$ports = $portsJson -join "`n" | ConvertFrom-Json
Write-Host ("listeners: " + $ports.listeners.Count + "  reserved: " + $ports.reserved.Count)
if ($ports.reserved.Count -gt 0) {
    Write-Host ("reserved[0]: " + $ports.reserved[0].start + "-" + $ports.reserved[0].end + " " + $ports.reserved[0].proto)
}

Write-Host '=== /kill (dummy victim) ==='
$victim = Start-Process -FilePath "$env:WINDIR\System32\cmd.exe" -ArgumentList '/k title kpt-cli-victim' -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1
$killJson = cmd /c "`"$exe`" /kill $($victim.Id)"
$kill = $killJson -join "`n" | ConvertFrom-Json
Write-Host ("killed: " + ($kill.killed -join ','))
Start-Sleep -Seconds 1
$still = Get-Process -Id $victim.Id -ErrorAction SilentlyContinue
Write-Host ("victim alive after kill: " + [bool]$still)
if ($still) { Stop-Process -Id $victim.Id -Force }
Write-Host 'done'
