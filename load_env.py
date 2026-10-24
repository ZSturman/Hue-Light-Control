Import("env")
import os

# Turns .env lines (KEY=value) into C string macros, e.g.
# WIFI_SSID=Home  ->  -DWIFI_SSID="Home"

REQUIRED_KEYS = ["WIFI_SSID", "WIFI_PASSWORD", "HUE_USERNAME", "HUE_GROUP_ID"]

env_file = os.path.join(env["PROJECT_DIR"], ".env")

if not os.path.exists(env_file):
    print("WARNING: .env file not found (copy .env.example to .env)")
    print("         Firmware will build but show 'Settings missing'.")
else:
    loaded = set()

    with open(env_file) as f:
        for line in f:
            line = line.strip()

            if not line or line.startswith("#") or "=" not in line:
                continue

            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip()

            # Allow KEY="value with spaces" as well as KEY=value.
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]

            if not value:
                continue

            env.Append(
                CPPDEFINES=[
                    (key, env.StringifyMacro(value))
                ]
            )

            loaded.add(key)

    missing = [key for key in REQUIRED_KEYS if key not in loaded]

    if missing:
        print("WARNING: .env is missing: " + ", ".join(missing))

    if "HUE_BRIDGE_IP" not in loaded:
        print("NOTE: HUE_BRIDGE_IP not set; the device will search for the bridge.")

    print("Loaded environment variables from .env")
