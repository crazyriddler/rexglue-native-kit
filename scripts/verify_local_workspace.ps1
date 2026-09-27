$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

Write-Host "== Local workspace verification =="
Write-Host "Root: $root"

$required = @(
    "CLAUDE.md",
    "START_PROMPT.md",
    ".claude\settings.json",
    ".claude\skills",
    ".claude\agents",
    "docs\PROJECT_STATE.md"
)

foreach ($item in $required) {
    if (Test-Path $item) { Write-Host "[OK] $item" }
    else { Write-Host "[MISSING] $item" }
}

if (Get-Command claude -ErrorAction SilentlyContinue) {
    Write-Host "[OK] Claude Code: $(& claude --version)"
} else {
    Write-Host "[MISSING] Claude Code in PATH"
}

if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Host "[OK] Git: $(& git --version)"
    if (Test-Path ".git") {
        Write-Host "[INFO] Local Git repository detected"
        $remotes = & git remote -v 2>$null
        if ($remotes) {
            Write-Host "[INFO] Existing remotes (not required by this kit):"
            $remotes | ForEach-Object { Write-Host "  $_" }
        } else {
            Write-Host "[OK] No remote configured; local Git works normally"
        }
    } else {
        Write-Host "[INFO] No .git directory. Git is optional; Claude may run git init locally."
    }
} else {
    Write-Host "[INFO] Git not found. Claude can still work on the local files; checkpoints will use other methods until Git is installed."
}
