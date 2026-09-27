Set-Location -LiteralPath $PSScriptRoot

if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
    Write-Error "Claude Code is not available in PATH."
    exit 1
}

Write-Host "ReXGlue Native Port Kit - Claude Code"
Write-Host "Project: $PWD"
Write-Host "GitHub remote: not required"
Write-Host "Mode: bypassPermissions"

$prompt = Get-Content -Raw -LiteralPath ".\START_PROMPT.md"
& claude --permission-mode bypassPermissions $prompt
