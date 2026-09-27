Set-Location -LiteralPath $PSScriptRoot

if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
    Write-Error "Claude Code no esta disponible en PATH."
    exit 1
}

Write-Host "ReXGlue Native Port Kit - Claude Code"
Write-Host "Proyecto: $PWD"
Write-Host "GitHub remoto: no requerido"
Write-Host "Modo: bypassPermissions"

$prompt = Get-Content -Raw -LiteralPath ".\START_PROMPT.md"
& claude --permission-mode bypassPermissions $prompt
