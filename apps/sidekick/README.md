# Sidekick AI Tutor

Sidekick is the hackathon app for a camera and voice enabled AI tutor on the
Tuya T5AI board.

This app is the product scaffold. Keep hardware bring-up experiments in
`examples/` and use this app for the integrated tutor experience.

## Current Scope

- Register T5AI board hardware.
- Start camera preview on the LCD when the camera/LCD module is enabled.
- Open microphone input and count captured PCM frames.
- Start in Hint Coach mode by default.
- Poll the touch panel and cycle tutor modes on touch:
  `Active -> Hint -> Summary`.
- Keep tutor orchestration as a small state-machine placeholder.
- Defer speaker validation until the external speaker module is available.

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
