<#
.SYNOPSIS
    Run Gear 360 ap_device steps on the Raspberry Pi from your PC and get a status report.

.DESCRIPTION
    1. Syncs ap_device folder to the Pi (optional).
    2. Connects the Pi's WiFi to the camera (using config/camera_wifi.conf on Pi).
    3. Starts the stream relay on the Pi.
    Reports Pi IP and stream URL at the end.

.PARAMETER PiHost
    Pi hostname or IP (default: from config/pc_defaults.ps1 or 10.0.0.210).

.PARAMETER PiUser
    SSH user on the Pi (default: from config or "roman").

.PARAMETER SkipSync
    Do not scp ap_device to the Pi (use existing copy).

.PARAMETER SkipWifi
    Do not run connect_camera_wifi.sh (already connected).

.PARAMETER SkipRelay
    Do not start the relay (only sync and/or wifi).

.PARAMETER RelayPort
    Port for the relay (default: 7679).

.PARAMETER ListWifi
    Only list WiFi networks visible from the Pi (to compare with camera SSID), then exit.

.PARAMETER LaunchViewer
    After setup, launch the Gear 360 viewer on this PC to display the stream.

.EXAMPLE
    .\run_from_pc.ps1
    .\run_from_pc.ps1 -PiHost 10.0.0.210 -PiUser roman
    .\run_from_pc.ps1 -SkipSync
#>

param(
    [string] $PiHost = "",
    [string] $PiUser = "",
    [switch] $SkipSync,
    [switch] $SkipWifi,
    [switch] $SkipRelay,
    [switch] $ListWifi,
    [switch] $LaunchViewer,
    [int]    $RelayPort = 7679
)

$ErrorActionPreference = "Stop"
$ScriptDir = $PSScriptRoot
$ApDeviceDir = $ScriptDir
# If run from ap_device, parent of ap_device is SamsungGear360
$SamsungGear360Dir = (Get-Item $ScriptDir).Parent.FullName

# Load defaults
$DefaultsPath = Join-Path $ScriptDir "config\pc_defaults.ps1"
if (Test-Path $DefaultsPath) {
    . $DefaultsPath
}
if (-not $script:PI_HOST) { $script:PI_HOST = "10.0.0.210" }
if (-not $script:PI_USER) { $script:PI_USER = "roman" }
if (-not $script:PI_AP_DEVICE_PATH) { $script:PI_AP_DEVICE_PATH = "~/ap_device" }

if ($PiHost) { $script:PI_HOST = $PiHost }
if ($PiUser) { $script:PI_USER = $PiUser }

$report = @()
function Report-Step($name, $ok, $detail) {
    $status = if ($ok) { "OK" } else { "FAILED" }
    $line = "  [$status] $name"
    if ($detail) { $line += " - $detail" }
    $script:report += $line
    Write-Host $line -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
}

Write-Host "`n--- Gear 360 ap_device: run from PC ---" -ForegroundColor Cyan
Write-Host "  Pi: $($script:PI_USER)@$($script:PI_HOST)`n"
Write-Host "  (To avoid repeated password prompts, set up SSH keys: ssh-copy-id $($script:PI_USER)@$($script:PI_HOST))" -ForegroundColor DarkGray
Write-Host ""

$piTarget = "$($script:PI_USER)@$($script:PI_HOST)"

$sshArgs = @(
    "-o", "ConnectTimeout=10"
)
$scpArgs = @(
    "-o", "ConnectTimeout=10"
)

function Ensure-SshKeyAuth() {
    $sshDir = Join-Path $HOME ".ssh"
    $keyPath = Join-Path $sshDir "id_ed25519"
    $pubPath = "${keyPath}.pub"

    if (-not (Test-Path $sshDir)) {
        New-Item -ItemType Directory -Path $sshDir -Force | Out-Null
    }

    if (-not (Test-Path $keyPath) -or -not (Test-Path $pubPath)) {
        Write-Host "Generating SSH key (ed25519)..." -ForegroundColor Cyan
        ssh-keygen -t ed25519 -N "" -f $keyPath | Out-Null
    }

    # If BatchMode works, we already have key auth (or agent) set up.
    # IMPORTANT: with $ErrorActionPreference='Stop', a failing native ssh call can terminate the script.
    # Treat any failure here as "no key auth yet" and proceed to install the key.
    $batchOk = $false
    $prevEap = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & ssh @sshArgs -o BatchMode=yes $piTarget "echo ok" 2>$null | Out-Null
        $batchOk = ($LASTEXITCODE -eq 0)
    } catch {
        $batchOk = $false
    } finally {
        $ErrorActionPreference = $prevEap
    }
    if ($batchOk) { return }

    # Install our public key onto the Pi (one-time password prompt).
    $pub = (Get-Content -Raw $pubPath).Trim()
    if (-not $pub) { throw "Could not read public key at $pubPath" }

    Write-Host "Installing SSH key on Pi (you may be prompted once for password)..." -ForegroundColor Cyan
    $installCmd = @"
set -e
mkdir -p ~/.ssh
touch ~/.ssh/authorized_keys
chmod 700 ~/.ssh
chmod 600 ~/.ssh/authorized_keys
grep -qxF '$pub' ~/.ssh/authorized_keys || echo '$pub' >> ~/.ssh/authorized_keys
echo ok
"@
    & ssh @sshArgs $piTarget $installCmd | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install SSH key on Pi."
    }
}

try {
    Ensure-SshKeyAuth

# ListWifi: only show what the Pi sees, then exit
if ($ListWifi) {
    Write-Host "Listing WiFi networks visible from the Pi..." -ForegroundColor Cyan
    Write-Host "  (Your config expects SSID: Gear 360(B8F1))" -ForegroundColor Gray
    Write-Host ""
    $ap = $script:PI_AP_DEVICE_PATH
    $listCmd = 'cd ' + $ap + ' 2>/dev/null; if [ -f scripts/list_wifi.sh ]; then sed -i ''s/\r$//'' scripts/list_wifi.sh 2>/dev/null; chmod +x scripts/list_wifi.sh; bash scripts/list_wifi.sh; else nmcli -t -f SSID,SIGNAL device wifi list 2>/dev/null | while IFS=: read -r s n; do echo "  SSID: [$s]  Signal: ${n}%"; done; fi'
    ssh @sshArgs $piTarget $listCmd
    Write-Host ""
    Write-Host "If the camera SSID is missing or different (e.g. no space), update config/camera_wifi.conf to match exactly." -ForegroundColor Yellow
    exit 0
}

# 1. Sync ap_device to Pi
if (-not $SkipSync) {
    try {
        Push-Location $SamsungGear360Dir
        scp -r @scpArgs "ap_device" "${piTarget}:~/"
        Pop-Location
        Report-Step "Sync ap_device to Pi" $true "~/ap_device updated"
        # Fix Windows CRLF on Pi so scripts execute (shebang "#!/bin/bash\r" fails); use tr (more portable than sed \x0d)
        try {
            $fixCmd = 'cd ' + $script:PI_AP_DEVICE_PATH + ' && for f in scripts/connect_camera_wifi.sh scripts/list_wifi.sh scripts/start_relay.sh scripts/relay_stream.py; do tr -d ''\r'' < "$f" > "$f.n" 2>/dev/null && mv "$f.n" "$f"; done; for f in config/camera_wifi.conf; do [ -f "$f" ] && tr -d ''\r'' < "$f" > "$f.n" 2>/dev/null && mv "$f.n" "$f"; done; chmod +x scripts/connect_camera_wifi.sh scripts/list_wifi.sh scripts/start_relay.sh'
            ssh @sshArgs $piTarget $fixCmd 2>&1 | Out-Null
        } catch { }
    } catch {
        Pop-Location -ErrorAction SilentlyContinue
        Report-Step "Sync ap_device to Pi" $false $_.Exception.Message
    }
} else {
    Report-Step "Sync ap_device" $true "skipped (SkipSync)"
}

# 2. Connect Pi WiFi to camera
if (-not $SkipWifi) {
    try {
        $out = ssh @sshArgs $piTarget "cd $($script:PI_AP_DEVICE_PATH) && chmod +x scripts/connect_camera_wifi.sh && ./scripts/connect_camera_wifi.sh" 2>&1
        $ok = $LASTEXITCODE -eq 0
        Report-Step "Connect Pi WiFi to camera" $ok $(if ($ok) { "connected" } else { $out })
        if (-not $ok -and "$out" -match "No network with SSID") {
            Write-Host "  If the stream already works, run with -SkipWifi. To fix SSID: -ListWifi then update config/camera_wifi.conf" -ForegroundColor Yellow
        }
    } catch {
        Report-Step "Connect Pi WiFi to camera" $false $_.Exception.Message
    }
} else {
    Report-Step "Connect Pi WiFi" $true "skipped (SkipWifi)"
}

# 3. Start relay on Pi (kill any existing relay first, then use start_relay.sh)
if (-not $SkipRelay) {
    try {
        # Ensure previous relay is gone so the new one can bind and we run latest code
        ssh @sshArgs $piTarget "pkill -9 -f relay_stream.py 2>/dev/null; sleep 2" 2>&1 | Out-Null
        $out = ssh @sshArgs $piTarget "cd $($script:PI_AP_DEVICE_PATH) && chmod +x scripts/start_relay.sh 2>/dev/null; bash scripts/start_relay.sh $RelayPort" 2>&1
        $started = "$out" -match "started"
        Report-Step "Start stream relay (port $RelayPort)" $started $(if ($started) { "running" } else { "see log below" })
        if (-not $started) {
            Write-Host "  relay.log:" -ForegroundColor Gray
            $out | ForEach-Object { Write-Host "    $_" -ForegroundColor Gray }
        }
        # Verify updated relay is serving (X-Relay-Version: 2 = Content-Length stripped)
        if ($started) {
            try {
                $checkUrl = "http://$($script:PI_HOST):$RelayPort/livestream_high.avi"
                $headers = Invoke-WebRequest -Uri $checkUrl -Method Head -TimeoutSec 8 -UseBasicParsing -ErrorAction SilentlyContinue
                $ver = $headers.Headers["X-Relay-Version"]
                if ($ver -ne "2") {
                    Write-Host "  [WARN] Relay may be old version (no X-Relay-Version: 2). Restart on Pi: pkill -f relay_stream.py; cd ~/ap_device; bash scripts/start_relay.sh $RelayPort" -ForegroundColor Yellow
                }
            } catch {
                Write-Host "  [WARN] Could not verify relay version (stream may not be ready yet)." -ForegroundColor Yellow
            }
        }
    } catch {
        Report-Step "Start stream relay" $false $_.Exception.Message
    }
} else {
    Report-Step "Start relay" $true "skipped (SkipRelay)"
}

# 4. Get Pi IP for report
$piIp = $script:PI_HOST
if ($script:PI_HOST -match "^\d+\.\d+\.\d+\.\d+$") {
    $piIp = $script:PI_HOST
} else {
    try {
        $piIp = (ssh @sshArgs $piTarget "hostname -I | awk '{print \$1}'" 2>$null).Trim()
        if (-not $piIp) { $piIp = $script:PI_HOST }
    } catch {
        $piIp = $script:PI_HOST
    }
}

$streamUrl = "http://${piIp}:${RelayPort}/livestream_high.avi"

Write-Host ""
Write-Host "--- Report ---" -ForegroundColor Cyan
$script:report | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Stream URL (open on your PC):" -ForegroundColor Yellow
Write-Host "  $streamUrl" -ForegroundColor White
Write-Host ""
Write-Host "Viewer:  python python/gear360_viewer.py $streamUrl" -ForegroundColor Gray
Write-Host "FFplay:  ffplay -fflags nobuffer -flags low_delay -i `"$streamUrl`"" -ForegroundColor Gray
Write-Host ""

if ($LaunchViewer) {
    $viewerScript = Join-Path $SamsungGear360Dir "python\gear360_viewer.py"
    if (Test-Path $viewerScript) {
        Write-Host "Launching viewer..." -ForegroundColor Cyan
        Start-Process -FilePath "python" -ArgumentList $viewerScript, $streamUrl -WorkingDirectory $SamsungGear360Dir
    } else {
        Write-Host "Viewer not found at $viewerScript" -ForegroundColor Yellow
    }
}

} finally {
}
