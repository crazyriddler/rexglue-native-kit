# RAM of the game process after a delay: mem_probe.ps1 <delay_s> [process name without .exe]
param([double]$Delay = 50, [string]$Name = "")
if (-not $Name) { $Name = (Get-Content "$PSScriptRoot/../kit.env" | Where-Object { $_ -match "^GAME_NAME=" }) -replace "^GAME_NAME=","" -replace "\s.*","" }
Start-Sleep -Seconds $Delay
$p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p) { "ram_working_set_mb={0:N0} private_mb={1:N0}" -f ($p.WorkingSet64/1MB), ($p.PrivateMemorySize64/1MB) }
