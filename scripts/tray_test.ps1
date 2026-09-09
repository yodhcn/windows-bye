$exe = 'dist\windows-bye\windows-bye.exe'
# 先读一次当前 Run 键（对照用，不修改）
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$before = (Get-ItemProperty -Path $runKey -ErrorAction SilentlyContinue).WindowsBye
Write-Host "Run.WindowsBye BEFORE: [$before]"

# 启动 --tray 模式
$p = Start-Process -FilePath $exe -ArgumentList '--tray' -WorkingDirectory 'dist\windows-bye' -PassThru
Start-Sleep -Seconds 6
if ($p.HasExited) {
    Write-Host "TRAY EXITED rc=$($p.ExitCode)"
    exit 1
} else {
    Write-Host "TRAY alive pid=$($p.Id) (无主界面，托盘驻留)"
    Stop-Process -Id $p.Id -Force
    Write-Host "tray mode OK"
}