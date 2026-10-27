param(
    [switch]$CheckOnly,
    [switch]$RestoreOnly,
    [switch]$FromLauncher
)

$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$package = Split-Path $tools -Parent
. (Join-Path $tools 'client-config.ps1')
$settingsPath = Join-Path $package 'config.json'
$launcherStatePath = Join-Path $package 'work\launcher-state.json'

function Read-Settings {
    Read-ClientConfig $settingsPath
}

function Test-LanternProbe([string]$Path, [long]$ExpectedSize) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $item = Get-Item -LiteralPath $Path
    if ($ExpectedSize -gt 0 -and $item.Length -ne $ExpectedSize) { return $false }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 2 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { return $false }
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    foreach ($marker in @('Lantern', 'UI viewport', 'BB_CONFIG', 'live_resolution')) {
        if (-not $ascii.Contains($marker)) { return $false }
    }
    return $true
}

function Write-Utf8NoBom([string]$Path, [string[]]$Lines) {
    [IO.File]::WriteAllLines($Path, $Lines, (New-Object Text.UTF8Encoding $false))
}

function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding $false))
}

function Set-JsonProperty($Object, [string]$Name, $Value) {
    if ($null -eq $Object.PSObject.Properties[$Name]) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    } else {
        $Object.$Name = $Value
    }
}

function Get-JsonPropertyState($Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return [pscustomobject]@{ exists = $false; value = $null } }
    [pscustomobject]@{ exists = $true; value = $property.Value }
}

function Restore-JsonProperty($Object, [string]$Name, $State) {
    if ([bool]$State.exists) { Set-JsonProperty $Object $Name $State.value }
    else { $Object.PSObject.Properties.Remove($Name) }
}

function Get-IniKeyState([string]$Path, [string]$Key) {
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*)$'
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding UTF8)) {
        if ($line -match $pattern) { return [pscustomobject]@{ exists = $true; value = $Matches[1] } }
    }
    [pscustomobject]@{ exists = $false; value = $null }
}

function Set-IniValues([string]$Source, [string]$Destination, [Collections.IDictionary]$Values) {
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $Source -Encoding UTF8)) { $lines.Add($line) }
    foreach ($key in $Values.Keys) {
        $pattern = '^\s*' + [regex]::Escape([string]$key) + '\s*='
        $found = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match $pattern) {
                $lines[$i] = "${key}=$($Values[$key])"
                $found = $true
                break
            }
        }
        if (-not $found) { $lines.Add("${key}=$($Values[$key])") }
    }
    Write-Utf8NoBom $Destination $lines.ToArray()
}

function Restore-IniKey([string]$Path, [string]$Key, $State) {
    if ([bool]$State.exists) {
        Set-IniValues $Path $Path ([ordered]@{ $Key = [string]$State.value })
        return
    }
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    $lines = @(Get-Content -LiteralPath $Path -Encoding UTF8 | Where-Object { $_ -notmatch $pattern })
    Write-Utf8NoBom $Path $lines
}

function Resolve-ServerIPv4([string]$HostName) {
    $candidate = [Net.Dns]::GetHostAddresses($HostName) |
        Where-Object { $_.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork } |
        Select-Object -First 1
    if ($null -eq $candidate) { throw "Could not resolve an IPv4 address for $HostName" }
    $candidate.IPAddressToString
}

function Save-LatestSessionLog([string]$GamePath, [string]$PackagePath) {
    $source = Join-Path $GamePath 'logs\last_session.log'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { return }
    $destination = Join-Path $PackagePath 'work\latest-session.log'
    try {
        Copy-Item -LiteralPath $source -Destination $destination -Force
        Write-Host "Latest Lantern session log saved to: $destination" -ForegroundColor Gray
    } catch {
        Write-Warning "Could not save the latest Lantern session log: $($_.Exception.Message)"
    }

    $overrideSource = Join-Path $GamePath 'network\host_overrides.json'
    if (Test-Path -LiteralPath $overrideSource -PathType Leaf) {
        $overrideDestination = Join-Path $PackagePath 'work\latest-host-overrides.json'
        try {
            Copy-Item -LiteralPath $overrideSource -Destination $overrideDestination -Force
            Write-Host "Runtime server mapping saved to: $overrideDestination" -ForegroundColor Gray
        } catch {
            Write-Warning "Could not save the runtime server mapping: $($_.Exception.Message)"
        }
    }
}

function Restore-LauncherState {
    if (-not (Test-Path -LiteralPath $launcherStatePath -PathType Leaf)) { return }
    $saved = Get-Content -LiteralPath $launcherStatePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (Test-Path -LiteralPath ([string]$saved.settingsPath) -PathType Leaf) {
        $launcherSettings = Get-Content -LiteralPath ([string]$saved.settingsPath) -Raw -Encoding UTF8 | ConvertFrom-Json
        $savedMethod = if ([bool]$saved.launch_method.exists) { [string]$saved.launch_method.value } else { '' }
        $savedArgs = if ([bool]$saved.custom_args.exists) { [string]$saved.custom_args.value } else { '' }
        $savedOurTemporaryHook =
            $savedMethod -eq 'custom' -and
            $savedArgs.IndexOf($PSCommandPath, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
            $savedArgs.IndexOf('-FromLauncher', [StringComparison]::OrdinalIgnoreCase) -ge 0
        if ($savedOurTemporaryHook) {
            # A previous interrupted recovery can save our own temporary hook as the "original"
            # launcher state. Never restore that recursive hook.
            Set-JsonProperty $launcherSettings 'launch_method' 'windows'
            Set-JsonProperty $launcherSettings 'custom_command' ''
            Set-JsonProperty $launcherSettings 'custom_args' ''
        } else {
            Restore-JsonProperty $launcherSettings 'launch_method' $saved.launch_method
            Restore-JsonProperty $launcherSettings 'custom_command' $saved.custom_command
            Restore-JsonProperty $launcherSettings 'custom_args' $saved.custom_args
        }
        Write-Json ([string]$saved.settingsPath) $launcherSettings
    }
    if (Test-Path -LiteralPath ([string]$saved.iniPath) -PathType Leaf) {
        Restore-IniKey ([string]$saved.iniPath) 'online_enabled' $saved.online_enabled
        Restore-IniKey ([string]$saved.iniPath) 'online_pool' $saved.online_pool
    }
    Remove-Item -LiteralPath $launcherStatePath -Force
    Write-Host 'Lantern launch method and online switch were restored.' -ForegroundColor Green
}

function Restore-StagedFiles([pscustomobject]$Paths) {
    $manifestPath = Join-Path $Paths.State 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        Write-Host 'No staged Lantern files need restoring.' -ForegroundColor Gray
        return
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $savedProbe = Join-Path $Paths.State 'original-bb-probe.exe'
    if (-not (Test-Path -LiteralPath $savedProbe)) { throw "Recovery manifest exists but the saved Lantern engine is missing: $savedProbe" }
    $expectedSize = if ($null -ne $settings.PSObject.Properties['supportedProbeSize']) {
        [long]$settings.supportedProbeSize
    } else { 0 }
    if (-not (Test-LanternProbe $savedProbe $expectedSize)) {
        throw "The saved recovery copy is not a supported Lantern engine: $savedProbe"
    }
    # The BBSC launcher lacks Lantern's native markers. A current native Lantern probe is
    # preserved even if Lantern regenerated it with a different size; otherwise restore the
    # known compatible recovery copy. This deliberately performs no file-hash check.
    if (-not (Test-LanternProbe $Paths.Probe 0)) {
        Copy-Item -LiteralPath $savedProbe -Destination $Paths.Probe -Force
    } else {
        Write-Host 'A native Lantern probe is already installed; preserving it.' -ForegroundColor Gray
    }
    foreach ($item in $manifest.staged) {
        $destination = Join-Path $Paths.Out ([string]$item.name)
        $priorFile = Join-Path $Paths.Prior ([string]$item.name)
        if ([bool]$item.existed) {
            if (-not (Test-Path -LiteralPath $priorFile)) { throw "Recovery copy is missing: $priorFile" }
            Copy-Item -LiteralPath $priorFile -Destination $destination -Force
        } elseif (Test-Path -LiteralPath $destination) {
            Remove-Item -LiteralPath $destination -Force
        }
    }
    $priorEngine = Join-Path $Paths.State 'prior-engine'
    foreach ($item in @($manifest.engineStaged)) {
        $destination = Join-Path $Paths.Engine ([string]$item.name)
        $priorFile = Join-Path $priorEngine ([string]$item.name)
        if ([bool]$item.existed) {
            if (-not (Test-Path -LiteralPath $priorFile)) { throw "Recovery copy is missing: $priorFile" }
            Copy-Item -LiteralPath $priorFile -Destination $destination -Force
        } elseif (Test-Path -LiteralPath $destination) {
            Remove-Item -LiteralPath $destination -Force
        }
    }
    $override = Join-Path $Paths.GameRoot 'network\host_overrides.json'
    $priorOverride = Join-Path $Paths.State 'host_overrides.json'
    if ([bool]$manifest.overrideExisted) { Copy-Item -LiteralPath $priorOverride -Destination $override -Force }
    elseif (Test-Path -LiteralPath $override) { Remove-Item -LiteralPath $override -Force }
    Remove-Item -LiteralPath $manifestPath -Force
    Write-Host 'Lantern engine and its original online-server file were restored.' -ForegroundColor Green
}

$settings = Read-Settings
$gameRoot = [IO.Path]::GetFullPath([string]$settings.gameRoot)
$accountFile = if ([IO.Path]::IsPathRooted([string]$settings.accountFile)) {
    [IO.Path]::GetFullPath([string]$settings.accountFile)
} else {
    [IO.Path]::GetFullPath((Join-Path $package ([string]$settings.accountFile)))
}
$outDir = Join-Path $gameRoot 'engine\out'
$probe = Join-Path $outDir 'bb-probe.exe'
$bbStart = Join-Path $outDir 'bb-start.exe'
$state = Join-Path $package 'work\state'
$prior = Join-Path $state 'prior'
$priorEngine = Join-Path $state 'prior-engine'
$hostDir = Join-Path $package 'work\host'
$hostProbe = Join-Path $state 'original-bb-probe.exe'
$bbscDir = Join-Path $package 'bbsc'
$config = Join-Path $package 'work\lantern-bbsc.ini'
$lanternIni = Join-Path $gameRoot 'lantern.ini'
$launcherExe = Join-Path $gameRoot 'Lantern Engine.exe'
$lanternSettings = Join-Path $env:APPDATA 'Lantern Engine\settings.json'
$engineDir = Join-Path $gameRoot 'engine'
$paths = [pscustomobject]@{
    GameRoot = $gameRoot; Engine = $engineDir; Out = $outDir; Probe = $probe
    Injector = (Join-Path $bbscDir 'bbsc-launch.exe'); State = $state; Prior = $prior
}

foreach ($required in @(
    $launcherExe, $lanternIni, $lanternSettings, $probe, $bbStart,
    (Join-Path $bbscDir 'bbsc-launch.exe'), (Join-Path $bbscDir 'bbsc.dll'),
    (Join-Path $bbscDir 'LICENSE.txt'), (Join-Path $bbscDir 'NOTICE.txt'),
    (Join-Path $bbscDir 'SUPPORTERS.txt'), $accountFile
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required file is missing: $required" }
}

$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ProcessName -in @('bb-probe', 'bb-start', 'Bloodborne', 'Lantern Engine')
})
if ($FromLauncher) {
    $engineRunning = @($running | Where-Object { $_.ProcessName -in @('bb-probe', 'bb-start', 'Bloodborne') })
    if ($engineRunning) { throw 'A Bloodborne engine process is already running.' }
} elseif ($running -and -not $CheckOnly) {
    throw 'Bloodborne or Lantern is running. Exit the game and launcher before starting or restoring BBSC.'
}

New-Item -ItemType Directory -Force -Path $state, $prior, $priorEngine, $hostDir, (Split-Path $config -Parent) | Out-Null
if (-not $CheckOnly -and -not $FromLauncher) {
    Restore-StagedFiles $paths
    Restore-LauncherState
}
if ($RestoreOnly) { exit 0 }

$expectedSize = if ($null -ne $settings.PSObject.Properties['supportedProbeSize']) {
    [long]$settings.supportedProbeSize
} else { 0 }
$probeToValidate = $probe
$pendingManifest = Join-Path $state 'manifest.json'
if ($CheckOnly -and -not (Test-LanternProbe $probe $expectedSize) -and (Test-Path -LiteralPath $pendingManifest -PathType Leaf)) {
    $savedProbe = Join-Path $state 'original-bb-probe.exe'
    if (Test-LanternProbe $savedProbe $expectedSize) {
        $probeToValidate = $savedProbe
        Write-Warning 'A previous BBSC run still needs cleanup. PLAY will recover it automatically before launch.'
    }
}
if (-not (Test-LanternProbe $probeToValidate $expectedSize)) {
    throw "The installed bb-probe.exe is not the supported Lantern $($settings.supportedLanternVersion) engine shape."
}
Write-Host 'Lantern probe validation passed: PE shape, size and Lantern markers.'
$account = Get-Content -LiteralPath $accountFile -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $account.name -or -not $account.password -or -not $account.server) { throw "The BBSC account file is incomplete: $accountFile" }
$address = [string]$settings.serverHost
$shadnetPort = [int]$settings.ports.shadnet
if ([string]$account.server -ne "${address}:${shadnetPort}") { throw 'Account belongs to another server. Run PLAY to select or register an account for config.json.' }
$connectAddress = Resolve-ServerIPv4 $address
$webApiPort = [int]$settings.ports.webApi
$gameApiPort = [int]$settings.ports.gameApi
$relayPort = [int]$settings.ports.relay
$udpPort = [int]$settings.ports.udp

$online = [ordered]@{
    online_enabled = '1'; online_connected = '1'; online_pool = ''
    online_host = $connectAddress; online_port = $shadnetPort
    online_webapi = "http://${connectAddress}:${webApiPort}"; online_game_api = "http://${connectAddress}:${gameApiPort}"
    online_username = [string]$account.name; online_remember = '0'
}
Set-IniValues $lanternIni $config $online
$fullscreenState = Get-IniKeyState $config 'fullscreen'
$fullscreenArg = if ($fullscreenState.exists -and ([string]$fullscreenState.value).Trim() -eq '1') { 'true' } else { 'false' }
$outputState = Get-IniKeyState $config 'output_res'
$outputDescription = if ($outputState.exists) { [string]$outputState.value } else { 'engine default' }
Write-Host "Lantern config copied for BBSC: $config" -ForegroundColor Green
Write-Host "Server: $address -> $connectAddress  Account: $($account.name)" -ForegroundColor Green
Write-Host "Lantern fullscreen setting forwarded to the BBSC engine: $fullscreenArg" -ForegroundColor Green
Write-Host "Lantern output preserved for BBSC: $outputDescription" -ForegroundColor Green
Write-Host 'Native Lantern probe selected; no replacement rendering core will be used.' -ForegroundColor Green
Write-Host 'Only BBSC injection and online server fields are added. Graphics, controller, mods, patches and save paths stay under Lantern control.' -ForegroundColor Gray

if ($CheckOnly) {
    Write-Host 'Compatibility check passed. No game files were changed.' -ForegroundColor Green
    exit 0
}

if (-not $FromLauncher) {
    $launcherConfig = Get-Content -LiteralPath $lanternSettings -Raw -Encoding UTF8 | ConvertFrom-Json
    $savedLauncher = [pscustomobject]@{
        settingsPath = $lanternSettings; iniPath = $lanternIni
        launch_method = Get-JsonPropertyState $launcherConfig 'launch_method'
        custom_command = Get-JsonPropertyState $launcherConfig 'custom_command'
        custom_args = Get-JsonPropertyState $launcherConfig 'custom_args'
        online_enabled = Get-IniKeyState $lanternIni 'online_enabled'
        online_pool = Get-IniKeyState $lanternIni 'online_pool'
    }
    Write-Json $launcherStatePath $savedLauncher

    $powerShellExe = (Get-Process -Id $PID).Path
    $childArgs = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -FromLauncher'
    Set-JsonProperty $launcherConfig 'launch_method' 'custom'
    Set-JsonProperty $launcherConfig 'custom_command' $powerShellExe
    Set-JsonProperty $launcherConfig 'custom_args' $childArgs
    Write-Json $lanternSettings $launcherConfig
    Set-IniValues $lanternIni $lanternIni ([ordered]@{ online_enabled = '0' })

    try {
        Write-Host 'Starting through Lantern Engine 0.9.37...' -ForegroundColor Green
        $launcher = Start-Process -FilePath $launcherExe -ArgumentList '--play' -WorkingDirectory $gameRoot -PassThru -Wait
        if ($launcher.ExitCode -ne 0) { throw "Lantern launcher exited with code $($launcher.ExitCode)" }
    } finally {
        Restore-LauncherState
    }
    exit 0
}

$stagedNames = @('bbsc.dll', 'LICENSE.txt', 'NOTICE.txt', 'SUPPORTERS.txt', 'CREDITS.txt', 'mod.json')
$manifestItems = @()
Copy-Item -LiteralPath $probe -Destination (Join-Path $state 'original-bb-probe.exe') -Force
foreach ($name in $stagedNames) {
    $destination = Join-Path $outDir $name
    $source = Join-Path $bbscDir $name
    if (-not (Test-Path -LiteralPath $source)) { continue }
    $existed = Test-Path -LiteralPath $destination
    if ($existed) { Copy-Item -LiteralPath $destination -Destination (Join-Path $prior $name) -Force }
    $manifestItems += [pscustomobject]@{ name = $name; existed = $existed }
}
$override = Join-Path $gameRoot 'network\host_overrides.json'
$overrideExisted = Test-Path -LiteralPath $override
if ($overrideExisted) { Copy-Item -LiteralPath $override -Destination (Join-Path $state 'host_overrides.json') -Force }
Write-Json (Join-Path $state 'manifest.json') ([pscustomobject]@{
    staged = $manifestItems; engineStaged = @(); overrideExisted = $overrideExisted
})

foreach ($name in $stagedNames) {
    $source = Join-Path $bbscDir $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $outDir $name) -Force }
}
Copy-Item -LiteralPath (Join-Path $bbscDir 'bbsc-launch.exe') -Destination $probe -Force

$secure = $account.password | ConvertTo-SecureString
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try { $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }

$oldPath = $env:PATH
$oldConfig = $env:BB_CONFIG
$oldEmulator = $env:BBSC_EMULATOR
$oldPassword = $env:BB_SHADNET_PASSWORD
$oldName = $env:BB_USER_NAME
$oldRelay = $env:BBSC_RELAY
$oldSeamless = $env:BBSC_MOD_SEAMLESS
$oldOnline = $env:BB_ONLINE
$oldServer = $env:BB_SHADNET_SERVER
$oldWebApi = $env:BB_SHADNET_WEBAPI
$oldCommunity = $env:BB_COMMUNITY_SERVER
$oldNpid = $env:BB_SHADNET_NPID
$oldFullscreen = $env:BB_FULLSCREEN
$oldGpuLog = $env:BB_GPU_LOG
$oldDlssDir = $env:BB_DLSS_DIR
$oldP2pChannel = $env:SHADPS4_BBSC_P2P_CHANNEL
$oldGameRelay = $env:BBSC_GAME_RELAY
$oldMultiplayerRelay = $env:BBSC_MULTIPLAYER_RELAY
$oldLanternCheats = $env:BBSC_LANTERN_CHEATS
try {
    $env:PATH = "$outDir;$oldPath"
    $env:BB_CONFIG = $config
    $env:BBSC_EMULATOR = $hostProbe
    # The saved native probe runs outside engine\out; retain Lantern's DLSS DLL lookup path.
    if ([string]::IsNullOrWhiteSpace($env:BB_DLSS_DIR) -and (Test-Path -LiteralPath (Join-Path $outDir 'nvngx_dlss.dll'))) {
        $env:BB_DLSS_DIR = $outDir
    }
    $env:BB_ONLINE = '1'
    $env:BB_SHADNET_SERVER = "${connectAddress}:${shadnetPort}"
    $env:BB_SHADNET_WEBAPI = "http://${connectAddress}:${webApiPort}"
    $env:BB_COMMUNITY_SERVER = "http://${connectAddress}:${gameApiPort}"
    $env:BB_SHADNET_NPID = [string]$account.name
    $env:BB_SHADNET_PASSWORD = $password
    $env:BB_USER_NAME = [string]$account.name
    $env:BBSC_RELAY = "${connectAddress}:${relayPort}"
    $env:BBSC_MOD_SEAMLESS = '1'
    $env:BBSC_LANTERN_CHEATS = '1'
    # The addressed mod channel owns game-facing endpoint translation; keep the old
    # optional peer channel disabled. Lantern's native signaling is left intact.
    $env:SHADPS4_BBSC_P2P_CHANNEL = '0'
    # BBU2 gives each peer a distinct logical address and forwards game packets to
    # exactly that peer over the existing server UDP port.
    $env:BBSC_GAME_RELAY = "${connectAddress}:${udpPort}"
    $env:BBSC_MULTIPLAYER_RELAY = if ($settings.multiplayerRelay) { '1' } else { '0' }
    $env:BB_FULLSCREEN = if ($fullscreenArg -eq 'true') { '1' } else { '0' }
    # Info records the actual ss.info URL, HTTP status and body size so a server-status failure
    # remains diagnosable without changing any Lantern rendering setting.
    $env:BB_GPU_LOG = 'info'
    if ([string]::IsNullOrWhiteSpace($env:BB_GAME_DIR)) {
        $env:BB_GAME_DIR = (Get-Content -LiteralPath (Join-Path $outDir 'game_dir.txt') -Raw).Trim()
    }
    Write-Host 'Injecting BBSC 0.0.2 into the native Lantern engine (BBU2 addressed multiplayer relay)...' -ForegroundColor Green
    Push-Location (Join-Path $gameRoot 'engine')
    try {
        & $bbStart '--game-dir' $env:BB_GAME_DIR
        $engineExit = $LASTEXITCODE
    } finally {
        Pop-Location
        Save-LatestSessionLog $gameRoot $package
    }
    if ($engineExit -ne 0) { throw "Lantern engine exited with code $engineExit" }
} finally {
    $password = $null
    $env:PATH = $oldPath
    $env:BB_CONFIG = $oldConfig
    $env:BBSC_EMULATOR = $oldEmulator
    $env:BB_SHADNET_PASSWORD = $oldPassword
    $env:BB_USER_NAME = $oldName
    $env:BBSC_RELAY = $oldRelay
    $env:BBSC_MOD_SEAMLESS = $oldSeamless
    $env:BB_ONLINE = $oldOnline
    $env:BB_SHADNET_SERVER = $oldServer
    $env:BB_SHADNET_WEBAPI = $oldWebApi
    $env:BB_COMMUNITY_SERVER = $oldCommunity
    $env:BB_SHADNET_NPID = $oldNpid
    $env:BB_FULLSCREEN = $oldFullscreen
    $env:BB_GPU_LOG = $oldGpuLog
    $env:BB_DLSS_DIR = $oldDlssDir
    $env:SHADPS4_BBSC_P2P_CHANNEL = $oldP2pChannel
    $env:BBSC_GAME_RELAY = $oldGameRelay
    $env:BBSC_MULTIPLAYER_RELAY = $oldMultiplayerRelay
    $env:BBSC_LANTERN_CHEATS = $oldLanternCheats
    Restore-StagedFiles $paths
}
