# Defaults for ap_device/run_from_pc.ps1 (edit these on your PC)

# Raspberry Pi SSH host/IP (this is where `10.0.0.210` was coming from if you didn't pass -PiHost)
$script:PI_HOST = "10.0.0.210"

# Raspberry Pi SSH username
$script:PI_USER = "roman"

# Remote folder synced via scp (usually ~/ap_device)
$script:PI_AP_DEVICE_PATH = "~/ap_device"

# Optional: expected /24 LAN prefix for sanity-checking (example: your home LAN is 10.0.0.0/24).
# Leave empty to disable the warning.
$script:EXPECTED_LAN_PREFIX = ""
