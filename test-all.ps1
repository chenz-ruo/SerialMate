param(
    [string]$Port = "COM7",
    [ValidateRange(1, 4096)][int]$PayloadKiB = 1024
)

$ErrorActionPreference = "Stop"
& "$PSScriptRoot\verify-release.ps1" -Port $Port -PayloadKiB $PayloadKiB
exit $LASTEXITCODE
