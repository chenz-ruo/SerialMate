param()

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$buildDirectory = Join-Path $projectRoot "build"
$distributionDirectory = Join-Path $projectRoot "dist\SerialMate"

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
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha256.ComputeHash($bytes)) -replace '-', '').ToUpperInvariant() }
    finally { $sha256.Dispose() }
}

$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot "CMakeLists.txt") -Encoding UTF8 -Raw
$versionMatch = [regex]::Match($cmakeText, 'project\([^\)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) { throw "Unable to read project version from CMakeLists.txt." }
$version = $versionMatch.Groups[1].Value

$buildFull = [System.IO.Path]::GetFullPath($buildDirectory)
$expectedBuild = [System.IO.Path]::GetFullPath((Join-Path $projectRoot "build"))
if (-not $buildFull.Equals($expectedBuild, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace unexpected build path: $buildFull"
}
if (Test-Path -LiteralPath $buildFull) {
    Remove-Item -LiteralPath $buildFull -Recurse -Force
}

cmake -S $projectRoot -B $buildDirectory -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$targets = @(
    "SerialMate",
    "SerialMateTests",
    "UpdateCheckerTests",
    "UpdateInstallerTests",
    "CommRecordTests",
    "CommViewTests",
    "TextCodecStreamTests",
    "SerialPortInfoTests",
    "LogWriterTests",
    "RawRxWriterTests",
    "RxIngressQueueTests",
    "SerialPortShutdownTests",
    "VersionConsistencyTests",
    "UiGeometryTests",
    "UiSmokeTests",
    "SerialLoopbackTests"
)
foreach ($target in $targets) {
    cmake --build $buildDirectory --config Release --target $target --parallel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
ctest --test-dir $buildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$distributionFull = [System.IO.Path]::GetFullPath($distributionDirectory)
$allowedPrefix = $projectRoot.TrimEnd('\') + '\dist\'
if (-not $distributionFull.StartsWith($allowedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace unexpected distribution path: $distributionFull"
}
if (Test-Path -LiteralPath $distributionFull) {
    Remove-Item -LiteralPath $distributionFull -Recurse -Force
}
New-Item -ItemType Directory -Path $distributionFull -Force | Out-Null

$builtExe = Join-Path $buildDirectory "Release\SerialMate.exe"
$releaseExe = Join-Path $distributionFull "SerialMate.exe"
Copy-Item -LiteralPath $builtExe -Destination $releaseExe -Force
foreach ($name in @("README.md", "BUILD.md", "TEST_RESULTS.md", "ARCHITECTURE.md")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $distributionFull -Force
}

$exe = Get-Item -LiteralPath $releaseExe
$sha = (Get-FileHash -LiteralPath $releaseExe -Algorithm SHA256).Hash.ToUpperInvariant()
$commit = (git -C $projectRoot rev-parse HEAD).Trim()
$tree = (git -C $projectRoot rev-parse 'HEAD^{tree}').Trim()
$buildInputFingerprint = Get-BuildInputFingerprint $projectRoot
$metadata = @"
BuildType=DeveloperBuild
HardwareVerified=false
Version=$version
Commit=$commit
Tree=$tree
BuildInputFingerprint=$buildInputFingerprint
Exe=SerialMate.exe
ExeSize=$($exe.Length)
ExeSha256=$sha
BuiltAt=$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))
"@
Write-Utf8NoBomCrlf (Join-Path $distributionFull "BUILD_METADATA.txt") $metadata

$giteeManifest = @"
version=$version
url=https://gitee.com/yycz/serial-mate/releases/download/v$version/SerialMate.exe
sha256=$sha
size=$($exe.Length)
"@
$githubManifest = @"
version=$version
url=https://github.com/chenz-ruo/SerialMate/releases/download/v$version/SerialMate.exe
sha256=$sha
size=$($exe.Length)
"@
Write-Utf8NoBomCrlf (Join-Path $projectRoot "update-manifest-gitee.txt") $giteeManifest
Write-Utf8NoBomCrlf (Join-Path $projectRoot "update-manifest-github.txt") $githubManifest
Copy-Item -LiteralPath (Join-Path $projectRoot "update-manifest-gitee.txt") -Destination $distributionFull -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "update-manifest-github.txt") -Destination $distributionFull -Force

Write-Host "Developer Release EXE: $releaseExe"
Write-Host "Version: $version"
Write-Host ("Size: {0} bytes ({1:N1} KiB)" -f $exe.Length, ($exe.Length / 1KB))
Write-Host "SHA-256: $sha"
Write-Host "HardwareVerified=false (run verify-release.ps1 before publishing)"
