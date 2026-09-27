param(
    [Parameter(Mandatory=$true)][string]$Title
)
$log = Join-Path $PSScriptRoot '..\docs\EXPERIMENT_LOG.md'
if (-not (Test-Path $log)) { New-Item -ItemType File -Path $log -Force | Out-Null }
$count = (Select-String -Path $log -Pattern '^### EXP-' -ErrorAction SilentlyContinue).Count
$id = '{0:D3}' -f $count
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
$entry = @"

### EXP-$id - $Title
- Date: $stamp
- Commit:
- Question:
- Hypothesis:
- Change/instrumentation:
- Build/run scenario:
- Metrics/evidence:
- Result:
- Conclusion:
- Keep/revert/follow-up:
"@
Add-Content -Path $log -Value $entry
Write-Host "Created EXP-$id in $log"
