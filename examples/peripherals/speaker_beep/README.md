# Speaker Beep Validation

This example validates T5AI speaker playback through the board-registered
`tdl_audio` path. It is intentionally separate from `audio_codecs`, which keeps
the stock record/playback sample unchanged.

## Build

```bash
cd examples/peripherals/speaker_beep
uv run python ../../../tos.py build
```

## Flash

Use the flash serial port shown by `ls /dev/cu.*`. On the Tuya AI 5 board we
validated flashing at a lower baud rate:

```bash
uv run python ../../../tos.py flash -p /dev/cu.usbmodemXXXX -b 460800
```

After reboot, the speaker should play a repeating tone and logs should contain:

```text
speaker beep test via tdl_audio_play
```
