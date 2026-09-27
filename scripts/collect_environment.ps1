$ErrorActionPreference = "SilentlyContinue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

function Probe-Command($name, $args = @("--version")) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if (-not $cmd) { return "$name: NOT FOUND" }
    $version = & $name @args 2>&1 | Select-Object -First 3 | Out-String
    return "$name: $($cmd.Source)`n$($version.Trim())"
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("Generated: $(Get-Date -Format o)")
$lines.Add("Project root: $root")
$lines.Add("PowerShell: $($PSVersionTable.PSVersion)")
$lines.Add("OS: $([System.Environment]::OSVersion.VersionString)")
$lines.Add("")
$lines.Add((Probe-Command "claude"))
$lines.Add("")
$lines.Add((Probe-Command "git"))
$lines.Add("")
$lines.Add((Probe-Command "cmake"))
$lines.Add("")
$lines.Add((Probe-Command "ninja"))
$lines.Add("")
$lines.Add((Probe-Command "dxc" @("-help")))
$lines.Add("")

$vswhereCandidates = @(
    "$env:ProgramFiles(x86)\Microsoft Visual Studio\Installer\vswhere.exe",
    "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
)
$vswhere = $vswhereCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($vswhere) {
    $lines.Add("vswhere: $vswhere")
    $lines.Add((& $vswhere -all -products * -format json 2>&1 | Out-String).Trim())
} else {
    $lines.Add("vswhere: NOT FOUND")
}

$lines.Add("")
$lines.Add("Git repository: $(Test-Path '.git')")
if (Test-Path '.git') {
    $lines.Add("Git branch: $(& git branch --show-current)")
    $remotes = & git remote -v
    if ($remotes) { $lines.Add("Git remotes:`n$($remotes | Out-String)") }
    else { $lines.Add("Git remotes: none") }
}

$out = Join-Path $root "docs\ENVIRONMENT_REPORT.txt"
$lines | Set-Content -LiteralPath $out -Encoding UTF8
Get-Content -LiteralPath $out
