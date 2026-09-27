$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$target = Join-Path $root "_research\upstream"
New-Item -ItemType Directory -Force -Path $target | Out-Null

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "Git no esta disponible. Claude puede usar WebSearch/WebFetch o descargar archivos por otros medios."
    exit 1
}

$repos = @{
    "rexglue-sdk" = "https://github.com/rexglue/rexglue-sdk.git"
    "XenosRecomp" = "https://github.com/hedge-dev/XenosRecomp.git"
    "UnleashedRecomp" = "https://github.com/hedge-dev/UnleashedRecomp.git"
    "reblue" = "https://github.com/zolaware/reblue.git"
    "reblue-XenosRecomp" = "https://github.com/zolaware/reblue-XenosRecomp.git"
}

foreach ($name in $repos.Keys) {
    $dest = Join-Path $target $name
    if (Test-Path $dest) {
        Write-Host "[SKIP] $name ya existe"
        continue
    }
    Write-Host "[CLONE] $name"
    git clone --depth 1 $repos[$name] $dest
}

Write-Host "Referencias disponibles en $target"
