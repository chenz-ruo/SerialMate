param(
    [switch]$Prompt
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security
$secureToken = $null
$plainToken = $null
$credentialOutput = $null

function Test-GiteeApiToken([string]$Token) {
    if ([string]::IsNullOrWhiteSpace($Token)) { return $false }
    try {
        $account = Invoke-RestMethod `
            -Uri "https://gitee.com/api/v5/user" `
            -Method Get `
            -Headers @{ Authorization = "Bearer $Token" } `
            -TimeoutSec 15
        return -not [string]::IsNullOrWhiteSpace($account.login)
    }
    catch {
        return $false
    }
}

if (-not $Prompt) {
    $processInfo = New-Object System.Diagnostics.ProcessStartInfo
    $processInfo.FileName = "git.exe"
    $processInfo.Arguments = "credential fill"
    $processInfo.UseShellExecute = $false
    $processInfo.RedirectStandardInput = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $processInfo.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($processInfo)
    $process.StandardInput.WriteLine("protocol=https")
    $process.StandardInput.WriteLine("host=gitee.com")
    $process.StandardInput.WriteLine("")
    $process.StandardInput.Close()
    if ($process.WaitForExit(15000)) {
        $credentialOutput = $process.StandardOutput.ReadToEnd()
        $plainToken = ([regex]::Match($credentialOutput, '(?m)^password=(.*)$')).Groups[1].Value.Trim()
    }
    else {
        $process.Kill()
    }
    if (-not (Test-GiteeApiToken $plainToken)) {
        $plainToken = $null
    }
}

if ([string]::IsNullOrWhiteSpace($plainToken)) {
    $promptText = [regex]::Unescape("\u8BF7\u7C98\u8D34\u65B0\u7684 Gitee Token")
    $secureToken = Read-Host $promptText -AsSecureString
}
$pointer = [IntPtr]::Zero
try {
    if ($secureToken) {
        $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureToken)
        $plainToken = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
    }
    if ([string]::IsNullOrWhiteSpace($plainToken)) {
        throw ([regex]::Unescape("Token \u4E0D\u80FD\u4E3A\u7A7A\u3002"))
    }
    if (-not (Test-GiteeApiToken $plainToken)) {
        throw ([regex]::Unescape("Gitee Token \u9A8C\u8BC1\u5931\u8D25\uFF0C\u8BF7\u786E\u8BA4\u4EE4\u724C\u672A\u64A4\u9500\u4E14\u5177\u6709\u4ED3\u5E93\u6743\u9650\u3002"))
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($plainToken)
    $protected = [Security.Cryptography.ProtectedData]::Protect(
        $bytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $directory = Join-Path ([Environment]::GetFolderPath("ApplicationData")) "SerialMate"
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $path = Join-Path $directory "gitee-token.dpapi"
    [IO.File]::WriteAllBytes($path, $protected)
    Write-Host ([regex]::Unescape("Gitee Token \u5DF2\u4F7F\u7528 DPAPI \u52A0\u5BC6\u4FDD\u5B58\uFF1B\u540E\u7EED\u53D1\u5E03\u65E0\u9700\u91CD\u590D\u8F93\u5165\u3002"))
}
finally {
    if ($pointer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
    }
    $plainToken = $null
    $credentialOutput = $null
    $bytes = $null
    $protected = $null
}
