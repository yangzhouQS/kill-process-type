# orphan-timer-test.ps1 - verify auto-clean timer (ASCII only)
# 1) spawn an orphan node (parent cmd exits immediately)
# 2) enable OrphanAutoEnable=1 interval=1min, launch GUI
# 3) wait 75s, verify orphan was auto-killed
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\kill-process-type.exe'
$build = Split-Path $exe

# spawn orphan via hidden cmd: cmd starts node then exits -> node orphaned
Start-Process -FilePath "$env:WINDIR\System32\cmd.exe" -ArgumentList '/c','start','/b','node','-e','"setTimeout(()=>{},600000)"' -WindowStyle Hidden
Start-Sleep -Seconds 4

$raw = cmd /c "`"$exe`" /orphans:nodepy"
$o = ($raw -join "`n") | ConvertFrom-Json
$v = @($o.orphans)[0]
if (-not $v) { Write-Host 'FAIL: no orphan node spawned'; exit 1 }
Write-Host ("victim: pid=" + $v.pid)

# enable auto clean (1 min), launch app
$ini = Join-Path $build 'kill-process-type.ini'
"[main]`r`nOrphanAutoEnable=1`r`nOrphanIntervalMin=1`r`n" | Set-Content $ini -Encoding ASCII
Start-Process -FilePath $exe -WorkingDirectory $build
Write-Host 'waiting 75s for timer...'
Start-Sleep -Seconds 75

$alive = [bool](Get-Process -Id $v.pid -ErrorAction SilentlyContinue)
Write-Host ("victim alive after timer: " + $alive + $(if ($alive) { '  <- FAIL' } else { '  <- AUTO-CLEAN OK' }))

Get-Process -Name kill-process-type -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $ini -ErrorAction SilentlyContinue
Write-Host 'done'
