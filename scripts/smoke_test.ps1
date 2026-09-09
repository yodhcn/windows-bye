$ErrorActionPreference = 'Stop'
$dist = Join-Path $PSScriptRoot '..\dist\windows-bye'
$exe  = Join-Path $dist 'windows-bye.exe'

if (-not (Test-Path $exe)) { Write-Error "exe not found: $exe" }

$p = Start-Process -FilePath $exe -WorkingDirectory $dist -PassThru
Start-Sleep -Seconds 8

if ($p.HasExited) {
    Write-Host "EXITED early, rc=$($p.ExitCode)"
    exit 1
} else {
    Write-Host "ALIVE pid=$($p.Id) (window running)"
    # give it a moment, then verify still alive before closing
    Start-Sleep -Seconds 2
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
        Write-Host "OK: process launched, stayed alive, then terminated cleanly"
        exit 0
    } else {
        Write-Host "EXITED after startup, rc=$($p.ExitCode)"
        exit 1
    }
}