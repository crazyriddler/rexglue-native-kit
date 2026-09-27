$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "Git no esta instalado o no esta en PATH."
}

if (-not (Test-Path ".git")) {
    git init
}

Write-Host "Repositorio Git LOCAL preparado. No se ha configurado ningun remote."
git status --short
