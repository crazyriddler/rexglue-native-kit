Set-Location -LiteralPath $PSScriptRoot
if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
    Write-Error "Claude Code is not available in PATH."
    exit 1
}
$prompt = Get-Content -Raw -LiteralPath ".\RESUME_PROMPT.md"
& claude --permission-mode bypassPermissions $prompt
