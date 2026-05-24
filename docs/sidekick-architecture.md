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

## Component Interaction Graph

```mermaid
flowchart LR
    User["Student"] --> UI["Board UI<br/>Home, Kick, Settings, Tests"]
    UI --> Tutor["Tutor session<br/>Active, Hint, Summary"]
    Tutor --> Client["Firmware backend client"]

    subgraph Board["T5AI board firmware"]
        UI
        Tutor
        Client
        Camera["Camera / JPEG capture"]
        Audio["Audio codec<br/>mic count + PCM playback"]
    end

    subgraph Config["Build-time config"]
        Env["apps/sidekick/.env.local"]
        Gen["gen_sidekick_device_config.py"]
        Header["sidekick_device_config.h"]
    end

    subgraph Backend["Laptop backend"]
        Server["Go server :8787"]
        Frame["/sidekick/frame"]
        EndSession["/sidekick/session/end"]
        TTS["/sidekick/tts"]
        Cache["TTS cache<br/>memory + audio_cache/"]
    end

    subgraph Services["AI and speech services"]
        Ollama["Ollama vision<br/>llama3.2-vision:11b"]
        OpenAI["OpenAI TTS"]
        ElevenLabs["ElevenLabs TTS"]
    end

    Env --> Gen --> Header --> Client
    Tutor -->|capture interval reached| Camera
    Camera -->|JPEG bytes| Client
    Client -->|Wi-Fi HTTP POST| Frame
    Client -->|session end| EndSession
    Frame -->|image context| Ollama
    EndSession -->|retained frames| Ollama
    Ollama -->|text decision JSON| Server
    Server -->|should_respond + message| Client
    Server -->|pre-generate speech when enabled| TTS
    Client -->|request pcm_16000 for message| TTS
    TTS -->|configured provider| OpenAI
    TTS -->|configured provider| ElevenLabs
    OpenAI --> Cache
    ElevenLabs --> Cache
    Cache -->|audio response| Client
    Client --> Audio
    Audio --> User
```

## Sequential Workflow

```mermaid
flowchart TD
    Setup["Configure laptop IP, Wi-Fi, backend env"] --> BackendRun["Run backend<br/>go run ."]
    BackendRun --> BuildFlash["Build and flash firmware"]
    BuildFlash --> Boot["Board boots<br/>UI + audio + backend worker"]
    Boot --> Start["Tap KICK and start session"]
    Start --> Tick["Tutor tick every 1s"]
    Tick -->|10/15/20/30s interval| Capture["Capture JPEG"]
    Capture --> Upload["POST /sidekick/frame"]
    Upload --> Analyze["Backend saves frame<br/>and calls Ollama"]
    Analyze --> Decision{"Response needed?"}
    Decision -->|no| Tick
    Decision -->|yes| Message["Return text message JSON"]
    Message --> Speech["POST /sidekick/tts<br/>text -> audio"]
    Speech --> Play["Board plays PCM audio"]
    Play --> Tick
    Start -->|tap END| Summary["POST /sidekick/session/end"]
    Summary --> SummaryAudio["Optional summary TTS"]
    SummaryAudio --> Done["Back to Kick flow"]
```

Speech-to-text is not implemented in the current backend routes. The backend
currently exposes health, frame upload, session summary, and text-to-speech
endpoints only.

## Fact-Checked Defaults

| Item | Current value |
| --- | --- |
| Ollama model | `llama3.2-vision:11b` |
| Backend routes | `GET /health`, `POST /sidekick/frame`, `POST /sidekick/session/end`, `POST /sidekick/tts` |
| Sliding context default | `SIDEKICK_CONTEXT_FRAMES=1` |
| TTS provider default | `SIDEKICK_TTS_PROVIDER=none` |
| TTS options | `openai`, `elevenlabs` |
| Firmware TTS request format | `pcm_16000` |
| Capture interval options | `10`, `15`, `20`, `30` seconds |
| STT / speech-to-text | Not implemented |

## Reproduction Commands And Runtime Values

```bash
# Laptop LAN IP for SIDEKICK_BACKEND_HOST.
ipconfig getifaddr en0

# Backend service.
ollama pull llama3.2-vision:11b
cd apps/sidekick/backend
go run .

# Firmware network config and build.
cd apps/sidekick
cp .env.example .env.local
# edit SIDEKICK_BACKEND_HOST, SIDEKICK_WIFI_SSID, SIDEKICK_WIFI_PSWD
uv run python ../../tos.py build

# Device port and flash.
ls /dev/cu.*
uv run python ../../tos.py flash -p /dev/cu.usbmodemXXXX
```

## Current Boot Flow

1. Initialize Tuya logging.
2. Register board hardware.
3. Initialize KV, software timers, and the work queue.
4. Initialize the tutor session in Hint mode with the default capture interval.
5. Start the LCD/touch UI.
6. Open microphone input and count captured PCM frames.
7. Play the startup chime when enabled.
8. Start the backend worker thread; Wi-Fi/network setup is deferred until the
   first upload.
9. Start camera preview only when enabled; otherwise the home screen owns the
   display.
10. Poll touch input and tick the tutor session state machine.

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
