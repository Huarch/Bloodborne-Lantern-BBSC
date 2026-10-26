function Read-ClientConfig([string]$Path = (Join-Path (Split-Path $PSScriptRoot -Parent) 'config.json')) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw 'Missing client/config.json. Copy config.example.json to config.json and edit that one file.'
    }
    $value = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    $name = [string]$value.serverHost
    if ([string]::IsNullOrWhiteSpace($name) -or $name -match '[\s/:@?#]' -or
        [Uri]::CheckHostName($name) -eq [UriHostNameType]::Unknown) {
        throw 'config.json: serverHost must be an IPv4 address or DNS name without a port.'
    }
    foreach ($key in @('shadnet','webApi','gameApi','relay','udp','stun')) {
        $port = $value.ports.$key
        if ($null -eq $port -or [string]$port -notmatch '^\d+$' -or [long]$port -lt 1 -or [long]$port -gt 65535) {
            throw "config.json: invalid ports.$key"
        }
    }
    foreach ($key in @('registrationKey','gameRoot','accountFile','supportedLanternVersion','supportedProbeSize','multiplayerRelay')) {
        if ($null -eq $value.PSObject.Properties[$key]) { throw "config.json: missing $key" }
    }
    if ($value.multiplayerRelay -isnot [bool]) { throw 'config.json: multiplayerRelay must be true or false.' }
    return $value
}
function Save-ClientConfig($Value, [string]$Path) {
    $temp = $Path + '.tmp'
    [IO.File]::WriteAllText($temp, ($Value | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
    Move-Item -LiteralPath $temp -Destination $Path -Force
}
