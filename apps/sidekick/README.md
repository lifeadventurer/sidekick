# Sidekick AI Tutor

Sidekick is the hackathon app for a camera and voice enabled AI tutor on the
Tuya T5AI board.

Repository overview: [../../README.md](../../README.md).

This app is the product scaffold. Keep hardware bring-up experiments in
`examples/` and use this app for the integrated tutor experience.

## Current Scope

- Register T5AI board hardware.
- Show the SideKick home screen on the LCD.
- Use touch on the home screen to cycle tutor modes:
  `Active -> Hint -> Summary`.
- Open microphone input and count captured PCM frames.
- Play a short startup chime through the speaker.
- Start in Hint Coach mode by default.
- Keep tutor orchestration as a small state-machine placeholder.
- Keep camera preview wired but disabled by default while the home screen owns
  the display.

## Device Wi-Fi and backend (`.env.local`)

The board does not read `.env` at runtime. Set network values in
`apps/sidekick/.env.local` (gitignored); they are baked in when you build.

```bash
cd apps/sidekick
cp .env.example .env.local
# edit SIDEKICK_BACKEND_HOST, SIDEKICK_WIFI_SSID, SIDEKICK_WIFI_PSWD
uv run python ../../tos.py build
```

Priority: defaults → `.env.example` → `.env` → `.env.local` → shell `export SIDEKICK_*`.

`SIDEKICK_BACKEND_HOST` must be your Mac’s LAN IP (not `localhost`). The Go
backend still uses `apps/sidekick/backend/.env.local` for Ollama/TTS.

## Build

From the repository root:

```bash
cd apps/sidekick
uv run python ../../tos.py build
```

## Flash

Replace the serial port with the local board port shown by `ls /dev/cu.*`.

```bash
uv run python ../../tos.py flash -p /dev/cu.usbmodemXXXX
```

## Notes

The app config assumes the Tuya T5AI board with the 3.5 inch LCD expansion
module and camera:

```text
CONFIG_BOARD_CHOICE_T5AI=y
CONFIG_TUYA_T5AI_BOARD_EX_MODULE_35565LCD=y
CONFIG_ENABLE_EX_MODULE_CAMERA=y
```
