$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$required = @(
  'CLAUDE.md',
  'START_PROMPT.md',
  '.claude\settings.json',
  'docs\PROJECT_STATE.md',
  'docs\EXPERIMENT_LOG.md',
  'docs\RENDERER_ANALYSIS.md',
  'docs\BENCHMARKS.csv'
)
$missing = @()
foreach ($item in $required) {
    if (-not (Test-Path (Join-Path $root $item))) { $missing += $item }
}
$skills = Get-ChildItem (Join-Path $root '.claude\skills') -Directory -ErrorAction SilentlyContinue
$agents = Get-ChildItem (Join-Path $root '.claude\agents') -Filter '*.md' -ErrorAction SilentlyContinue
if ($missing.Count -gt 0) {
    Write-Error ("Missing required files: " + ($missing -join ', '))
    exit 1
}
Write-Host "Kit OK. Skills: $($skills.Count). Agents: $($agents.Count)."
