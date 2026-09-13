param(
    [string]$Port = "COM7",
    [int]$TimeoutSeconds = 120,
    [switch]$RequirePhysicalAction
)

$ErrorActionPreference = "Stop"

function Get-PortDevice([string]$Name) {
    Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match ("\(" + [regex]::Escape($Name) + "\)") } |
        Select-Object -First 1
}

$before = Get-PortDevice $Port
if (-not $before) {
    Write-Error "$Port is not present in the Plug and Play port list."
    exit 2
}

Write-Host ("Baseline: {0}  {1}  {2}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"), $before.Status, $before.FriendlyName)
if ($RequirePhysicalAction) {
    $confirmation = Read-Host "This test requires a real USB unplug/reinsert. Enter PHYSICAL-HOTPLUG-OK when ready"
    if ($confirmation -ne "PHYSICAL-HOTPLUG-OK") {
        Write-Error "Physical action was not confirmed; this run is PnP-only."
        exit 3
    }
}
Write-Host "Keep the serial assistant connected to $Port. Monitoring starts now; unplug the USB cable, wait about 2 seconds, then plug it back in."

$deadline = (Get-Date).AddSeconds([Math]::Max(5, $TimeoutSeconds))
$seenAbsent = $false
$seenRestored = $false
while ((Get-Date) -lt $deadline) {
    $device = Get-PortDevice $Port
    $state = if ($device) { $device.Status } else { "ABSENT" }
    Write-Host ("{0}  {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"), $state)
    if (-not $device) { $seenAbsent = $true }
    if ($seenAbsent -and $device) { $seenRestored = $true; break }
    Start-Sleep -Milliseconds 500
}

if ($seenRestored) {
    Write-Host "PASS: $Port disappeared and was observed after re-insertion."
    exit 0
}
if ($seenAbsent) {
    Write-Error "$Port disappeared but did not return before timeout."
} else {
    Write-Error "Monitoring timed out without observing $Port disappear."
}
exit 1
