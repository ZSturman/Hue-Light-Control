# Hue Light Control

Physical Philips Hue room controller: ESP32, rotary encoder and SSD1306 OLED.
Preserve the responsive input/display loop and separate core-0 network task.

## Map and commands
- Firmware: `src/`; pin assignments and timings: `include/config.h`.
- Build configuration: `platformio.ini`; credentials: ignored `.env` loaded by `load_env.py`.
- Documentation: `README.md`, `docs/`; editable portfolio artwork: `docs/images/*.svg`.
- Build: `pio run` (or `~/.platformio/penv/bin/platformio run`).
- Upload: `pio run -t upload`; serial logs: `pio device monitor` (115200 baud).
- No automated hardware suite exists. Verify turn/tap/hold, diagnostics and recovery on the device when firmware changes.

## Change discipline
- Keep the pinned ESP32 PlatformIO platform unless an upgrade is explicitly required.
- Never publish `.env`, credentials, generated `.pio/` files or private activity telemetry.
- Keep wiring documentation consistent with `include/config.h`.
- Portfolio artwork is technical illustration, not photographic device evidence.
- Use Better Workspace's editor/service and reviewed sync for project metadata; never edit its private database directly.
- Update documentation and `docs/NEXT_STEPS.md` with validation evidence and any unfinished work.
