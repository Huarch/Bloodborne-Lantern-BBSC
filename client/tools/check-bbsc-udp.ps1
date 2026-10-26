param(
    [string]$ConfigPath = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'client-config.ps1')

function New-RandomBytes([int]$Count) {
    $bytes = New-Object byte[] $Count
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
    } finally {
        $rng.Dispose()
    }
    return $bytes
}

function Test-UdpEndpoint(
    [string]$ServerName,
    [int]$Port,
    [ValidateSet("stun", "shadnet")][string]$Mode
) {
    $addresses = [System.Net.Dns]::GetHostAddresses($ServerName) |
        Where-Object { $_.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork }
    $address = $addresses | Select-Object -First 1
    if ($null -eq $address) {
        return [pscustomobject]@{
            Target = "$ServerName`:$Port"; Mode = $Mode; Success = $false
            Milliseconds = $null; Detail = "No IPv4 address"
        }
    }

    $transaction = New-RandomBytes 12
    if ($Mode -eq "stun") {
        [byte[]]$packet = New-Object byte[] 20
        $packet[0] = 0x00; $packet[1] = 0x01
        $packet[2] = 0x00; $packet[3] = 0x00
        $packet[4] = 0x21; $packet[5] = 0x12; $packet[6] = 0xA4; $packet[7] = 0x42
        [Array]::Copy($transaction, 0, $packet, 8, 12)
    } else {
        # Current Lantern PS4 P2P framing: marker, valid/plain-datagram flags,
        # source vport 0xFFFF, destination vport 0xFFFF, then the 21-byte ping.
        [byte[]]$packet = New-Object byte[] 27
        $packet[0] = 0xFF; $packet[1] = 0x83
        $packet[2] = 0xFF; $packet[3] = 0xFF; $packet[4] = 0xFF; $packet[5] = 0xFF
        $packet[6] = 0x01
        $suffix = -join ((New-RandomBytes 4) | ForEach-Object { $_.ToString("x2") })
        $peerId = [System.Text.Encoding]::ASCII.GetBytes("netdiag_$suffix")
        [Array]::Copy($peerId, 0, $packet, 7, [Math]::Min(16, $peerId.Length))
    }

    $client = New-Object System.Net.Sockets.UdpClient `
        ([System.Net.Sockets.AddressFamily]::InterNetwork)
    $client.Client.ReceiveTimeout = 2000
    $endpoint = New-Object System.Net.IPEndPoint $address, $Port
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $lastError = "No valid reply"
    try {
        for ($attempt = 1; $attempt -le 2; $attempt++) {
            [void]$client.Send($packet, $packet.Length, $endpoint)
            try {
                $remote = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Any), 0
                [byte[]]$reply = $client.Receive([ref]$remote)
                $sameEndpoint = $remote.Address.Equals($address) -and $remote.Port -eq $Port
                $valid = $false
                if ($sameEndpoint -and $Mode -eq "shadnet") {
                    $valid = $reply.Length -eq 12 -and
                        $reply[0] -eq 0xFF -and $reply[1] -eq 0x83 -and
                        $reply[2] -eq 0xFF -and $reply[3] -eq 0xFF -and
                        $reply[4] -eq 0xFF -and $reply[5] -eq 0xFF
                } elseif ($sameEndpoint -and $reply.Length -ge 20) {
                    $valid = $reply[0] -eq 0x01 -and $reply[1] -eq 0x01
                    for ($index = 0; $valid -and $index -lt 12; $index++) {
                        if ($reply[8 + $index] -ne $transaction[$index]) { $valid = $false }
                    }
                }
                if ($valid) {
                    return [pscustomobject]@{
                        Target = "$ServerName`:$Port"; Mode = $Mode; Success = $true
                        Milliseconds = $timer.ElapsedMilliseconds; Detail = "$($reply.Length) bytes"
                    }
                }
                $lastError = "Invalid reply"
            } catch [System.Net.Sockets.SocketException] {
                $lastError = $_.Exception.Message
            }
        }
    } finally {
        $timer.Stop()
        $client.Dispose()
    }
    return [pscustomobject]@{
        Target = "$ServerName`:$Port"; Mode = $Mode; Success = $false
        Milliseconds = $timer.ElapsedMilliseconds; Detail = $lastError
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
        $ConfigPath = Join-Path $PSScriptRoot '..\config.json'
    }
    $resolvedConfig = (Resolve-Path -LiteralPath $ConfigPath).Path
    $settings = Read-ClientConfig $resolvedConfig
    $serverName = [string]$settings.serverHost

    Write-Host "Config: $resolvedConfig"
    Write-Host "BBSC server: $serverName"
    Write-Host ""

    $serverResults = @(
        Test-UdpEndpoint $serverName ([int]$settings.ports.stun) 'stun'
        Test-UdpEndpoint $serverName ([int]$settings.ports.udp) 'shadnet'
    )
    $controlResults = @(
        Test-UdpEndpoint "stun.cloudflare.com" 3478 "stun"
        Test-UdpEndpoint "stun.l.google.com" 19302 "stun"
    )

    Write-Host "BBSC server checks (required):"
    $serverResults | Format-Table -AutoSize
    Write-Host "Public STUN controls (reference only):"
    $controlResults | Format-Table -AutoSize

    if (@($serverResults | Where-Object { -not $_.Success }).Count -eq 0) {
        Write-Host "BBSC server UDP is reachable."
        exit 0
    }
    Write-Host "BBSC server UDP check failed."
    exit 1
} catch {
    Write-Error $_.Exception.Message
    exit 2
}
