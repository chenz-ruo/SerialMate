param(
    [string]$Remote = "gitee",
    [string]$TargetName = "SerialMate"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$remoteUrl = (git -C $projectRoot remote get-url $Remote).Trim()
$match = [regex]::Match($remoteUrl, '^https://gitee\.com/([^/]+)/([^/]+?)(?:\.git)?$')
if (-not $match.Success) {
    throw (([regex]::Unescape("\u65E0\u6CD5\u4ECE\u8FDC\u7AEF\u5730\u5740\u8BC6\u522B Gitee \u4ED3\u5E93\uFF1A")) + $remoteUrl)
}
$owner = $match.Groups[1].Value
$currentPath = $match.Groups[2].Value

$tokenPath = Join-Path ([Environment]::GetFolderPath("ApplicationData")) "SerialMate\gitee-token.dpapi"
if (-not (Test-Path -LiteralPath $tokenPath -PathType Leaf)) {
    throw ([regex]::Unescape("\u672A\u627E\u5230 DPAPI \u4EE4\u724C\uFF0C\u8BF7\u5148\u8FD0\u884C set-gitee-token.ps1\u3002"))
}

$tokenBytes = $null
$token = $null
$body = $null
try {
    $protectedBytes = [IO.File]::ReadAllBytes($tokenPath)
    $tokenBytes = [Security.Cryptography.ProtectedData]::Unprotect(
        $protectedBytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $token = [Text.Encoding]::UTF8.GetString($tokenBytes)
    if ([string]::IsNullOrWhiteSpace($token)) {
        throw ([regex]::Unescape("DPAPI \u4EE4\u724C\u4E3A\u7A7A\u3002"))
    }

    if ($currentPath -ne $TargetName) {
        $body = @{
            access_token = $token
            name = $TargetName
            path = $TargetName
            private = "false"
            default_branch = "main"
        } | ConvertTo-Json
        $repository = Invoke-RestMethod `
            -Uri "https://gitee.com/api/v5/repos/$owner/$currentPath" `
            -Method Patch `
            -Headers @{ Authorization = "Bearer $token" } `
            -ContentType "application/json" `
            -Body $body
    }
    else {
        $repository = Invoke-RestMethod -Uri "https://gitee.com/api/v5/repos/$owner/$TargetName" -Method Get
    }

    if ($repository.path -ne $TargetName -or $repository.private -ne $false) {
        throw ([regex]::Unescape("Gitee \u4ED3\u5E93\u6CA1\u6709\u8FBE\u5230\u76EE\u6807\u540D\u79F0\u6216\u516C\u5F00\u72B6\u6001\u3002"))
    }
    git -C $projectRoot remote set-url $Remote "https://gitee.com/$owner/$TargetName.git"
    if ($LASTEXITCODE -ne 0) {
        throw ([regex]::Unescape("\u66F4\u65B0\u672C\u5730 Gitee \u8FDC\u7AEF\u5730\u5740\u5931\u8D25\u3002"))
    }

    $successPrefix = [regex]::Unescape("Gitee \u4ED3\u5E93\u5DF2\u539F\u5730\u91CD\u547D\u540D\u4E3A ")
    $defaultBranchText = [regex]::Unescape("\uFF0C\u9ED8\u8BA4\u5206\u652F\u4E3A ")
    $successSuffix = [regex]::Unescape("\uFF0C\u516C\u5F00\u72B6\u6001\u5DF2\u786E\u8BA4\u3002")
    Write-Host ($successPrefix + $repository.full_name + $defaultBranchText + $repository.default_branch + $successSuffix)
}
finally {
    if ($tokenBytes) { [Array]::Clear($tokenBytes, 0, $tokenBytes.Length) }
    $protectedBytes = $null
    $tokenBytes = $null
    $token = $null
    $body = $null
}
