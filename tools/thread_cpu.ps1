# Per-thread CPU of the game process over an interval: thread_cpu.ps1 <seconds> [process name]
param([double]$Seconds = 10, [string]$Name = "")
if (-not $Name) { $Name = (Get-Content "$PSScriptRoot/../kit.env" | Where-Object { $_ -match "^GAME_NAME=" }) -replace "^GAME_NAME=","" -replace "\s.*","" }
$p = Get-Process $Name
$a = @{}; foreach ($t in $p.Threads) { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }
Start-Sleep -Seconds $Seconds
$p = Get-Process $Name
$rows = foreach ($t in $p.Threads) {
  $before = if ($a.ContainsKey($t.Id)) { $a[$t.Id] } else { 0 }
  [pscustomobject]@{ Id = $t.Id; Pct = [math]::Round(($t.TotalProcessorTime.TotalMilliseconds - $before) / ($Seconds * 10), 1) }
}
$rows | Sort-Object Pct -Descending | Select-Object -First 14 | ForEach-Object { "{0} {1}" -f $_.Id, $_.Pct }
