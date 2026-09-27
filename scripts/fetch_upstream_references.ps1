$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$target = Join-Path $root "_research\upstream"
New-Item -ItemType Directory -Force -Path $target | Out-Null

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "Git is not available. Use WebSearch/WebFetch or download the sources another way."
    exit 1
}

$repos = @{
    "rexglue-sdk" = "https://github.com/rexglue/rexglue-sdk.git"
    "XenosRecomp" = "https://github.com/hedge-dev/XenosRecomp.git"
    "UnleashedRecomp" = "https://github.com/hedge-dev/UnleashedRecomp.git"
    "reblue" = "https://github.com/zolaware/reblue.git"
    "reblue-XenosRecomp" = "https://github.com/zolaware/reblue-XenosRecomp.git"
    "plume" = "https://github.com/renderbag/plume.git"
    "skate3recomp" = "https://github.com/mchughalex/skate3recomp.git"
    "The-Darkness-Recomp" = "https://github.com/portingpete/The-Darkness-Recomp.git"
    "LostOdysseyRecomp" = "https://github.com/freefrank/LostOdysseyRecomp.git"
}

foreach ($name in $repos.Keys) {
    $dest = Join-Path $target $name
    if (Test-Path $dest) {
        Write-Host "[SKIP] $name already exists"
        continue
    }
    Write-Host "[CLONE] $name"
    git clone --depth 1 $repos[$name] $dest
}

Write-Host "References available in $target (see docs/UPSTREAM_RESEARCH.md for the inspected commits)"
