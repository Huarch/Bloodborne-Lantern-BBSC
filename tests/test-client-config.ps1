$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'client\tools\client-config.ps1')
$temp = Join-Path ([IO.Path]::GetTempPath()) ('bbsc-config-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $path = Join-Path $temp 'config.json'
    $config = Get-Content (Join-Path $root 'client\config.example.json') -Raw | ConvertFrom-Json
    $config.serverHost = '127.0.0.1'
    $config.ports.udp = 41314
    $config.ports.stun = 43478
    Save-ClientConfig $config $path
    $loaded = Read-ClientConfig $path
    if ($loaded.ports.udp -ne 41314 -or $loaded.ports.stun -ne 43478) { throw 'Custom ports lost' }
    foreach ($bad in @('https://example.invalid', 'user@example.invalid', 'example.invalid/path')) {
        $config.serverHost = $bad
        Save-ClientConfig $config $path
        $rejected = $false
        try { Read-ClientConfig $path | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'Invalid endpoint accepted' }
    }
    $config.serverHost = '127.0.0.1'
    $config.ports.udp = 65536
    Save-ClientConfig $config $path
    $rejected = $false
    try { Read-ClientConfig $path | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid port accepted' }
    foreach ($file in Get-ChildItem (Join-Path $root 'client\tools') -Filter '*.ps1') {
        $tokens=$null; $errors=$null
        [void][Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)
        if ($errors) { throw ($errors | Out-String) }
    }
    Write-Output 'CLIENT_SINGLE_CONFIG_TESTS_OK'
} finally {
    $resolved = [IO.Path]::GetFullPath($temp)
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'bbsc-config-test-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
