# Sidekick Architecture

Sidekick is an AI tutor firmware app for Tuya T5AI hardware with camera,
microphones, display, and optional speaker output.

## Project Layout

```text
apps/sidekick/
  app_default.config       T5AI board configuration for the app
  CMakeLists.txt           TuyaOpen app build registration
  src/                     Firmware entry point
  hardware/                Board hardware registration
  vision/                  Camera and display preview pipeline
  audio/                   Microphone capture pipeline
  ui/                      Touch input and mode switching
  tutor/                   Tutor session state and orchestration
```

## Component Graph

```mermaid
flowchart TD
    User[Student / User] --> UI[Sidekick UI]
    User --> AudioHW[Microphone / Speaker]
    User --> CameraHW[Camera]
    User --> TouchHW[Touch / LCD]

    subgraph App[Sidekick Firmware App]
        Main[sidekick_main.c<br/>boot + main loop]
        Hardware[Hardware Init]
        UI[UI<br/>mode + interval controls]
        Tutor[Tutor Session<br/>Active, Hint, Summary]
        Vision[Camera Pipeline<br/>JPEG capture + optional preview]
        Audio[Audio Pipeline<br/>mic frames + PCM playback]
        BackendClient[Backend Client<br/>upload + TTS playback]
        DeviceConfig[Generated Device Config<br/>Wi-Fi + laptop LAN IP]
        Config[App Config + Logging]
    end

    subgraph TuyaOpen[TuyaOpen SDK]
        TALSystem[TAL System<br/>threads, timers, workqueue, log]
        Peripherals[Peripherals<br/>camera, display, touch, audio]
        Network[Network Stack<br/>TAL network, Wi-Fi, netmgr]
        Libraries[Support Libraries<br/>HTTP, JSON, TLS, security]
    end

    subgraph BoardPlatform[Board / Platform / Build]
        Board[T5AI Board Package]
        Platform[T5AI Platform SDK]
        BuildTools[tos.py + CMake + Kconfig]
        ConfigGen[gen_sidekick_device_config.py]
    end

    subgraph Backend[Local Sidekick Backend]
        Laptop[Laptop LAN IP<br/>192.168.x.x:8787]
        GoServer[Go HTTP Server<br/>apps/sidekick/backend]
        Health[GET /health]
        Frame[POST /sidekick/frame]
        SessionEnd[POST /sidekick/session/end]
        TTS[POST /sidekick/tts]
        CaptureStore[Debug JPEG Saves<br/>captures/]
        SessionStore[Sliding Frame Context<br/>default 1 frame]
        Analyzer[Visual Tutor Analyzer]
        LLM[Ollama<br/>llama3.2-vision:11b]
        Fallback[Demo-safe Fallback<br/>on AI error]
        TTSPreGen[TTS Pre-generation]
        TTSCache[Memory + Disk Cache<br/>audio_cache/]
        TTSEngine[TTS Providers<br/>none by default, OpenAI or ElevenLabs by config]
    end

    BuildTools --> App
    BuildTools --> TuyaOpen
    BuildTools --> BoardPlatform
    BuildTools --> ConfigGen
    ConfigGen --> DeviceConfig

    Main --> Config
    Main --> Hardware
    Main --> BackendClient
    Main --> Tutor
    Main --> UI
    Main --> Audio
    Main --> Vision
    Main --> TALSystem

    Hardware --> Board
    Board --> Platform

    UI --> Tutor
    UI --> TouchHW
    UI --> Audio
    UI --> Vision
    UI --> Peripherals

    Audio --> AudioHW
    Audio --> Peripherals

    Vision --> CameraHW
    Vision --> TouchHW
    Vision --> Peripherals

    Tutor --> Vision
    Tutor --> Audio
    Tutor --> BackendClient

    DeviceConfig --> BackendClient
    BackendClient --> Vision
    BackendClient --> Audio
    BackendClient --> Network
    BackendClient --> Libraries
    Network --> Laptop
    Laptop --> GoServer

    GoServer --> Health
    GoServer --> Frame
    GoServer --> SessionEnd
    GoServer --> TTS
    Frame --> CaptureStore
    Frame --> SessionStore
    Frame --> Analyzer
    SessionEnd --> SessionStore
    SessionEnd --> Analyzer
    Analyzer --> LLM
    Analyzer --> Fallback
    Analyzer --> TTSPreGen
    TTSPreGen --> TTSCache
    TTS --> TTSEngine
    TTS --> TTSCache
    TTSEngine --> TTSCache
    TTSCache --> BackendClient
```

Speech-to-text is not implemented in the current backend routes. The backend
currently exposes health, frame upload, session summary, and text-to-speech
endpoints only.

## Current Boot Flow

1. Initialize Tuya logging.
2. Register board hardware.
3. Start camera preview on the LCD.
4. Start microphone capture.
5. Start in Hint Coach mode.
6. Poll touch input so a tap cycles modes: `Active -> Hint -> Summary`.
7. Run the tutor session state machine.

Speaker output is intentionally not part of the first scaffold because the
external speaker module still needs hardware validation.

## Build

From the repository root:

```bash
cd apps/sidekick
uv run python ../../tos.py build
```

## Flash

Use the serial port shown by `ls /dev/cu.*`.

```bash
uv run python ../../tos.py flash -p /dev/cu.usbmodemXXXX
```

## Near-Term Milestones

- Validate camera preview inside `apps/sidekick`.
- Validate microphone frame capture without relying on speaker playback.
- Add a push-to-talk or wake-word interaction state.
- Add network setup and AI service boundary.
- Add speaker response once the external speaker module is available.
