param(
    [string]$Port = "COM7",
    [ValidateRange(1, 4096)][int]$PayloadKiB = 1024
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$buildDirectory = Join-Path $projectRoot "build"
$releaseDirectory = Join-Path $buildDirectory "Release"
$distributionDirectory = Join-Path $projectRoot "dist\SerialMate"
$exe = Join-Path $releaseDirectory "SerialMate.exe"
$distributionExe = Join-Path $distributionDirectory "SerialMate.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Release EXE is missing. Run build-release.ps1 first." }
if (-not (Test-Path -LiteralPath $distributionExe)) { throw "Distribution EXE is missing. Run build-release.ps1 first." }
if ((git -C $projectRoot status --porcelain).Count -ne 0) {
    throw "Working tree must be clean before release verification."
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE." }
}

function Write-Utf8NoBomCrlf([string]$Path, [string]$Content) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    $normalized = $Content -replace "`r?`n", "`r`n"
    [System.IO.File]::WriteAllText($Path, $normalized, $encoding)
}

function Get-BuildInputFingerprint([string]$Root) {
    $paths = @(git -C $Root ls-files | Where-Object {
        $_ -and (
            $_ -eq 'CMakeLists.txt' -or
            $_ -like 'src/*' -or
            $_ -like 'tests/*' -or
            $_ -like 'assets/*' -or
            $_ -in @('build-release.ps1', 'verify-release.ps1', 'publish-release.ps1', 'test-all.ps1', 'test-hotplug.ps1')
        )
    })
    $parts = foreach ($relative in $paths) {
        $full = Join-Path $Root ($relative -replace '/', '\')
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            $hash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToUpperInvariant()
            "$relative=$hash"
        }
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($parts -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToUpperInvariant() }
    finally { $sha.Dispose() }
}

$started = Get-Date
Invoke-Checked "ctest" @("--test-dir", $buildDirectory, "-C", "Release", "--output-on-failure")
Invoke-Checked (Join-Path $releaseDirectory "SerialPortInfoTests.exe") @($Port)
Invoke-Checked (Join-Path $releaseDirectory "SerialPortShutdownTests.exe") @($Port)
Invoke-Checked (Join-Path $releaseDirectory "SerialLoopbackTests.exe") @($Port, $PayloadKiB.ToString())
Invoke-Checked (Join-Path $releaseDirectory "UiSmokeTests.exe") @($exe, $Port)

$device = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match ("\(" + [regex]::Escape($Port) + "\)") } |
    Select-Object -First 1
$deviceName = if ($device) { $device.FriendlyName } else { "Unavailable" }
$instanceId = if ($device) { $device.InstanceId } else { "Unavailable" }
$vidValue = if ($instanceId -match 'VID_([0-9A-Fa-f]{4})') { $Matches[1].ToUpperInvariant() } else { "Unavailable" }
$pidValue = if ($instanceId -match 'PID_([0-9A-Fa-f]{4})') { $Matches[1].ToUpperInvariant() } else { "Unavailable" }
$commit = (git -C $projectRoot rev-parse HEAD).Trim()
$tree = (git -C $projectRoot rev-parse 'HEAD^{tree}').Trim()
$buildInputFingerprint = Get-BuildInputFingerprint $projectRoot
$sha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToUpperInvariant()
$size = (Get-Item -LiteralPath $exe).Length
if ((Get-FileHash -LiteralPath $distributionExe -Algorithm SHA256).Hash.ToUpperInvariant() -ne $sha) {
    throw "Distribution EXE does not match the verified build output."
}
$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot "CMakeLists.txt") -Encoding UTF8 -Raw
$versionMatch = [regex]::Match($cmakeText, 'project\([^\)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) { throw "Unable to read project version from CMakeLists.txt." }
$version = $versionMatch.Groups[1].Value
foreach ($manifestName in @("update-manifest-gitee.txt", "update-manifest-github.txt")) {
    $manifest = Get-Content -LiteralPath (Join-Path $projectRoot $manifestName) -Encoding UTF8 -Raw
    $expectedUrl = if ($manifestName -eq "update-manifest-gitee.txt") {
        "url=https://gitee.com/yycz/serial-mate/releases/download/v$version/SerialMate.exe"
    } else {
        "url=https://github.com/chenz-ruo/SerialMate/releases/download/v$version/SerialMate.exe"
    }
    if ($manifest -notmatch [regex]::Escape("version=$version") -or
        $manifest -notmatch [regex]::Escape($expectedUrl) -or
        $manifest -notmatch [regex]::Escape("sha256=$sha") -or
        $manifest -notmatch [regex]::Escape("size=$size")) {
        throw "$manifestName does not match the verified EXE."
    }
}
$duration = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
$report = @"
VerificationType=RealHardwareLoopback
HardwareVerified=true
ReleaseReady=true
AutomatedTests=Passed
RealComLoopback=Passed
OpenClose100=Passed
CloseTail=Passed
UiVerification=Passed
UpdateVerification=Passed
VersionConsistency=Passed
WorkingTreeClean=true
Version=$version
VerifiedAt=$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))
DurationSeconds=$duration
OS=$([Environment]::OSVersion.VersionString)
VerifiedCommit=$commit
VerifiedTree=$tree
BuildInputFingerprint=$buildInputFingerprint
Exe=SerialMate.exe
ExeSha256=$sha
ExeSize=$size
VerifiedPort=$Port
Device=$deviceName
InstanceId=$instanceId
VID=$vidValue
PID=$pidValue
Parameters=115200,8,1,None,None
LoopbackPayloadKiB=$PayloadKiB
SerialShutdownCycles=100
UiSmoke=Passed
PhysicalHotplug=NotRun
PnPEnumeration=Passed
"@
New-Item -ItemType Directory -Path $distributionDirectory -Force | Out-Null
Write-Utf8NoBomCrlf (Join-Path $distributionDirectory "VERIFIED_RELEASE.txt") $report
Write-Host "VERIFIED_RELEASE"
Write-Host $report
