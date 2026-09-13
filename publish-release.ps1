param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$SkipGitHub,
    [switch]$SkipGitee
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$distributionDirectory = Join-Path $projectRoot "dist\SerialMate"
$exe = Join-Path $distributionDirectory "SerialMate.exe"
$verification = Join-Path $distributionDirectory "VERIFIED_RELEASE.txt"
$tag = "v$Version"
if (-not (Test-Path -LiteralPath $exe)) { throw "Release EXE is missing." }
if (-not (Test-Path -LiteralPath $verification)) { throw "VERIFIED_RELEASE.txt is missing." }
$verificationText = Get-Content -LiteralPath $verification -Encoding UTF8 -Raw
$head = (git -C $projectRoot rev-parse HEAD).Trim()
$verifiedCommit = ([regex]::Match($verificationText, '(?m)^VerifiedCommit=([0-9a-fA-F]+)\r?$')).Groups[1].Value
$verifiedFingerprint = ([regex]::Match($verificationText, '(?m)^BuildInputFingerprint=([0-9a-fA-F]+)\r?$')).Groups[1].Value
$physicalHotplug = ([regex]::Match($verificationText, '(?m)^PhysicalHotplug=([^\r\n]+)')).Groups[1].Value
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
$buildInputFingerprint = Get-BuildInputFingerprint $projectRoot
$sha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToUpperInvariant()
$verifiedCommitIsAncestor = $false
if (-not [string]::IsNullOrWhiteSpace($verifiedCommit)) {
    git -C $projectRoot merge-base --is-ancestor $verifiedCommit $head
    $verifiedCommitIsAncestor = $LASTEXITCODE -eq 0
}
if ($verificationText -notmatch '(?m)^HardwareVerified=true\r?$' -or
    $verificationText -notmatch '(?m)^AutomatedTests=Passed\r?$' -or
    $verificationText -notmatch '(?m)^COM7Loopback=Passed\r?$' -or
    $physicalHotplug -notin @('Passed', 'NotRun') -or
    [string]::IsNullOrWhiteSpace($verifiedCommit) -or
    -not $verifiedCommitIsAncestor -or
    $verifiedFingerprint -ne $buildInputFingerprint -or
    $verificationText -notmatch [regex]::Escape("ExeSha256=$sha")) {
    throw "Verification report does not match the current build inputs and EXE."
}
foreach ($manifestName in @("update-manifest-gitee.txt", "update-manifest-github.txt")) {
    $manifest = Get-Content -LiteralPath (Join-Path $projectRoot $manifestName) -Encoding UTF8 -Raw
    if ($manifest -notmatch [regex]::Escape("version=$Version") -or
        $manifest -notmatch [regex]::Escape("sha256=$sha")) {
        throw "$manifestName does not match the verified EXE."
    }
}
if ((git -C $projectRoot status --porcelain).Count -ne 0) { throw "Working tree must be clean before publishing." }

$productText = [regex]::Unescape("\u4E32\u53E3\u52A9\u624B")
$notes = "SerialMate $productText v$Version`r`n`r`nSee TEST_RESULTS.md for verification details."
if (-not $SkipGitHub) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw "GitHub CLI (gh) is not installed." }
    $remoteHead = ((gh api repos/chenz-ruo/SerialMate/git/ref/heads/main --jq '.object.sha') | Out-String).Trim()
    if ($remoteHead -ne $head) { throw "GitHub main does not point to the verified commit ($head)." }
    $oldErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & gh release view $tag --repo chenz-ruo/SerialMate 2>&1 | Out-Null
    $releaseExists = $LASTEXITCODE -eq 0
    $ErrorActionPreference = $oldErrorAction
    if ($releaseExists) {
        & gh release upload $tag $exe --repo chenz-ruo/SerialMate --clobber
    } else {
        & gh release create $tag $exe --repo chenz-ruo/SerialMate --title "SerialMate $tag" --notes $notes --target main
    }
    if ($LASTEXITCODE -ne 0) { throw "GitHub release publishing failed." }
}

if (-not $SkipGitee) {
    $giteeToken = $env:GITEE_TOKEN
    if ([string]::IsNullOrWhiteSpace($giteeToken)) {
        $tokenPath = Join-Path ([Environment]::GetFolderPath("ApplicationData")) "SerialMate\gitee-token.dpapi"
        if (Test-Path -LiteralPath $tokenPath -PathType Leaf) {
            try {
                $protected = [IO.File]::ReadAllBytes($tokenPath)
                $plain = [Security.Cryptography.ProtectedData]::Unprotect(
                    $protected, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
                $giteeToken = [Text.Encoding]::UTF8.GetString($plain)
            } catch {
                throw ([regex]::Unescape("\u65E0\u6CD5\u8BFB\u53D6\u52A0\u5BC6\u7684 Gitee Token\uFF0C\u8BF7\u91CD\u65B0\u8FD0\u884C set-gitee-token.ps1\u3002"))
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($giteeToken)) {
        throw ([regex]::Unescape("\u672A\u627E\u5230 Gitee Token\u3002\u9996\u6B21\u4F7F\u7528\u8BF7\u8FD0\u884C set-gitee-token.ps1\u3002"))
    }
    $base = "https://gitee.com/api/v5/repos/yycz/serial-mate"
    $headers = @{ Authorization = "Bearer $giteeToken" }
    try {
        $release = Invoke-RestMethod -Uri "$base/releases/tags/$tag" -Headers $headers -Method Get
    } catch {
        $release = $null
    }
    if ($null -eq $release -or $null -eq $release.id) {
        $body = @{ access_token = $giteeToken; tag_name = $tag; name = $tag; body = $notes; target_commitish = "main" } |
            ConvertTo-Json -Compress
        $release = Invoke-RestMethod -Uri "$base/releases" -Headers $headers -Method Post -ContentType "application/json" -Body $body
    }
    if ($null -eq $release -or $null -eq $release.id) { throw "Gitee release creation returned no release id." }
    Add-Type -AssemblyName System.Net.Http
    $client = New-Object System.Net.Http.HttpClient
    $client.DefaultRequestHeaders.Authorization = New-Object System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", $giteeToken)
    $form = New-Object System.Net.Http.MultipartFormDataContent
    $stream = [System.IO.File]::OpenRead($exe)
    try {
        $fileContent = New-Object System.Net.Http.StreamContent($stream)
        $form.Add($fileContent, "file", "SerialMate.exe")
        $form.Add((New-Object System.Net.Http.StringContent($giteeToken)), "access_token")
        $response = $client.PostAsync("$base/releases/$($release.id)/attach_files", $form).GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) { throw "Gitee asset upload failed: $($response.StatusCode)" }
    } finally {
        $stream.Dispose(); $form.Dispose(); $client.Dispose()
    }
}
Write-Host "Published $tag with SHA-256 $sha"
