param([switch]$CheckOnly, [switch]$SetupOnly)
$ErrorActionPreference = 'Stop'
$package = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'client-config.ps1')
$configPath = Join-Path $package 'config.json'
$settings = Read-ClientConfig $configPath
$serverAddress = [string]$settings.serverHost
$serverPort = [int]$settings.ports.shadnet
$serverEndpoint = "${serverAddress}:${serverPort}"
function Test-GameRoot([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    (Test-Path -LiteralPath (Join-Path $Path 'Lantern Engine.exe')) -and
    (Test-Path -LiteralPath (Join-Path $Path 'engine\out\bb-probe.exe')) -and
    (Test-Path -LiteralPath (Join-Path $Path 'lantern.ini'))
}
$candidates = [Collections.Generic.List[string]]::new()
foreach ($candidate in @($settings.gameRoot, $package, (Split-Path $package -Parent))) {
    if (-not [string]::IsNullOrWhiteSpace($candidate)) { $candidates.Add([string]$candidate) }
}
$launcherFile = Join-Path $env:APPDATA 'Lantern Engine\settings.json'
if (Test-Path -LiteralPath $launcherFile) {
    try {
        $launcher = Get-Content -LiteralPath $launcherFile -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($candidate in @($launcher.port_dir, $launcher.game_dir)) {
            if (-not [string]::IsNullOrWhiteSpace($candidate)) {
                $candidates.Add([string]$candidate)
                $candidates.Add((Split-Path ([string]$candidate) -Parent))
            }
        }
    } catch { }
}
$gameRoot = $candidates | Where-Object { Test-GameRoot $_ } | Select-Object -First 1
if (-not $gameRoot) {
    if ($CheckOnly) { throw 'Lantern not found. Run PLAY LANTERN + BBSC.cmd and select its folder.' }
    Add-Type -AssemblyName System.Windows.Forms
    $picker = New-Object Windows.Forms.FolderBrowserDialog
    $picker.Description = 'Select the Bloodborne folder containing Lantern Engine.exe'
    $picker.ShowNewFolderButton = $false
    try {
        if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 0 }
        $gameRoot = $picker.SelectedPath
    } finally { $picker.Dispose() }
    if (-not (Test-GameRoot $gameRoot)) { throw 'That folder does not contain an installed Lantern Engine.' }
}
$gameRoot = [IO.Path]::GetFullPath($gameRoot)
# Each Windows user gets a separate encrypted account, never bundled in the shared ZIP.
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$userKey = $sid -replace '[^A-Za-z0-9_-]', '_'
$accountFile = Join-Path $package ('work\accounts\'+$userKey+'.dat')
$account = $null
$accountCandidates = @($accountFile, (Join-Path $PSScriptRoot 'account.dat'))
if ($settings.accountFile) {
    $configuredAccount = if ([IO.Path]::IsPathRooted([string]$settings.accountFile)) { [string]$settings.accountFile } else { Join-Path $package ([string]$settings.accountFile) }
    $accountCandidates = @($configuredAccount) + $accountCandidates
}
$accountsDir = Join-Path $package 'work\accounts'
if (Test-Path -LiteralPath $accountsDir -PathType Container) {
    $accountCandidates += @(Get-ChildItem -LiteralPath $accountsDir -Filter '*.dat' -File | Select-Object -ExpandProperty FullName)
}
foreach ($candidate in @($accountCandidates | Select-Object -Unique)) {
    if (-not (Test-Path -LiteralPath $candidate)) { continue }
    try {
        $found = Get-Content -LiteralPath $candidate -Raw -Encoding UTF8 | ConvertFrom-Json
        $secure = $found.password | ConvertTo-SecureString
        if ($found.server -eq $serverEndpoint -and $found.name -and $secure.Length -gt 0) { $account = $found; $accountFile = $candidate; break }
    } catch { }
}
if (-not $account) {
    if ($CheckOnly) {
        Write-Host ('Server configured: '+$serverEndpoint)
        Write-Host ('Lantern found: '+$gameRoot)
        Write-Host 'First normal launch will create a separate account for this Windows user.'
        exit 0
    }
    if ([string]::IsNullOrWhiteSpace([string]$settings.registrationKey)) { throw 'Set registrationKey in config.json before first registration.' }
    $tcp = [Net.Sockets.TcpClient]::new()
    try {
        $connect = $tcp.ConnectAsync($serverAddress, [int]$serverPort)
        if (-not $connect.Wait(4000) -or -not $tcp.Connected) { throw 'Server is unreachable. Ask the host to check its cloud security group.' }
    } finally { $tcp.Dispose() }
    $helper = Join-Path $package 'account-helper\shadnet-sample.exe'
    if (-not (Test-Path -LiteralPath $helper)) { throw 'Account setup tool is missing. Extract the full ZIP first.' }
    $base = ($env:USERNAME -replace '[^A-Za-z0-9_-]', '').ToLowerInvariant()
    if ($base.Length -lt 3) { $base = 'hunter' }
    if ($base.Length -gt 10) { $base = $base.Substring(0,10) }
    $registered = $false
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $name = $base+'_'+([Guid]::NewGuid().ToString('N').Substring(0,4))
        $bytes = New-Object byte[] 24
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
        $password = [Convert]::ToBase64String($bytes)
        $reply = & $helper $serverAddress $serverPort 'register' $name $password ($name+'@bbsc.local') $settings.registrationKey 2>&1 | Out-String
        if ($reply -match 'Account created successfully') { $registered = $true; break }
        if ($reply -notmatch 'CreationExistingUsername|CreationExistingEmail') { break }
    }
    if (-not $registered) { $password = $null; throw 'Account creation failed. Ask the host to check the server address and registration key.' }
    $secure = ConvertTo-SecureString -String $password -AsPlainText -Force
    $account = [ordered]@{ server=$serverEndpoint; name=$name; password=($secure | ConvertFrom-SecureString) }
    $password = $null
    $reply = $null
    New-Item -ItemType Directory -Path (Split-Path $accountFile -Parent) -Force | Out-Null
    [IO.File]::WriteAllText($accountFile, ($account | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
    Write-Host ('Account created: '+$name)
}
$settings.gameRoot = $gameRoot
$settings.accountFile = $accountFile
Save-ClientConfig $settings $configPath
Write-Host ('Lantern-BBSC: '+$serverEndpoint+' / '+$account.name)
$play = Join-Path $PSScriptRoot 'play-lantern-bbsc.ps1'
if ($CheckOnly -or $SetupOnly) { & $play -CheckOnly } else { & $play }
