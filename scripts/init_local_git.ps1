$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "Git is not installed or not in PATH."
}

if (-not (Test-Path ".git")) {
    git init
}

Write-Host "Local Git repository ready. No remote configured (none is needed)."
git status --short
