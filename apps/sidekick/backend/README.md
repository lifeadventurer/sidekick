# SideKick Backend

Small local backend for SideKick AI experiments.

The firmware will POST JPEG frames over Wi-Fi to this service. The service calls
OpenAI `gpt-5.5` for visual tutoring, compares each frame with recent prior
frames, and returns compact JSON for the device UI.

## Run

OpenAI vision mode:

```bash
cd apps/sidekick/backend
cp .env.example .env.local
# Set OPENAI_API_KEY in .env.local.
go run .
```

Use verbose logs when you want to see what the model returned and what the backend
sent back to the device:

```bash
go run . -verbose
```

The server listens on:

```text
http://0.0.0.0:8787
```

From another terminal:

```bash
curl -s http://localhost:8787/health
curl -s -X POST --data-binary @frame.jpg -H 'Content-Type: image/jpeg' 'http://localhost:8787/sidekick/frame?mode=hint&session=demo'
```

The default analyzer uses the OpenAI Responses API with `gpt-5.5` and
`detail: "auto"` image input, which preserves original-detail images on
`gpt-5.5` while remaining compatible with alternate OpenAI vision models.
Ollama remains available as a local fallback:

```bash
SIDEKICK_AI_PROVIDER=ollama OLLAMA_MODEL=llama3.2-vision:11b go run .
```

When calling from the board, use the laptop LAN IP instead of `localhost`, for
example:

```text
http://192.168.1.50:8787/sidekick/frame?mode=hint
```

## Fake Mode

Fake mode is useful before Ollama is running or while wiring firmware upload:

```bash
cd apps/sidekick/backend
SIDEKICK_AI_PROVIDER=fake go run .
```

## Local Env

The backend automatically loads `.env.local` and then `.env` from this directory.
Shell environment variables win over file values. Keep `.env.local` uncommitted.

```bash
cp .env.example .env.local
```

Example `.env.local`:

```text
SIDEKICK_AI_PROVIDER=openai
OPENAI_API_KEY=your_openai_api_key_here
OPENAI_VISION_MODEL=gpt-5.5
SIDEKICK_TTS_PROVIDER=none
```

Optional environment variables:

| Name | Default | Description |
| --- | --- | --- |
| `PORT` | `8787` | HTTP listen port |
| `SIDEKICK_AI_PROVIDER` | `openai` | `openai`, `ollama`, or `fake` |
| `OPENAI_API_KEY` | empty | Required for OpenAI image analysis, transcription, or TTS |
| `OPENAI_VISION_MODEL` | `gpt-5.5` | OpenAI image analysis model |
| `OPENAI_RESPONSES_URL` | `https://api.openai.com/v1/responses` | OpenAI image analysis endpoint |
| `OLLAMA_MODEL` | `llama3.2-vision:11b` | Local Ollama vision model |
| `OLLAMA_CHAT_URL` | `http://localhost:11434/api/chat` | Ollama chat endpoint |
| `SIDEKICK_MAX_IMAGE_BYTES` | `4194304` | Max uploaded image size |
| `SIDEKICK_SHARED_SECRET` | empty | Optional bearer token required from firmware |
| `SIDEKICK_TIMEOUT_SECONDS` | `45` | Upstream model request timeout |
| `SIDEKICK_MAX_OUTPUT_TOKENS` | `256` | Max model output tokens, including OpenAI reasoning tokens |
| `SIDEKICK_CONTEXT_FRAMES` | `1` | Sliding window of recent frames retained per session |
| `SIDEKICK_AI_FALLBACK` | `1` | Return demo-safe fallback JSON instead of HTTP 502 when analysis fails (`0` disables) |
| `SIDEKICK_VERBOSE` | `0` | Enable verbose model and response logs (`go run . -verbose` overrides this) |
| `SIDEKICK_CAPTURE_DIR` | `captures` | Directory for incoming JPEG debug dumps (`off` to disable) |
| `SIDEKICK_STT_PROVIDER` | `elevenlabs` | `elevenlabs`, `openai`, `fake`, or `none` for microphone transcription |
| `SIDEKICK_MAX_AUDIO_BYTES` | `524288` | Max uploaded microphone PCM size |
| `SIDEKICK_STT_LANGUAGE` | `en` | Optional ISO-639-1 language hint for transcription |
| `ELEVENLABS_STT_MODEL` | `scribe_v2` | ElevenLabs speech-to-text model |
| `OPENAI_TRANSCRIPTION_MODEL` | `gpt-4o-mini-transcribe` | OpenAI speech-to-text model |
| `SIDEKICK_TTS_PROVIDER` | `none` | `none`, `openai`, or `elevenlabs` |
| `SIDEKICK_TTS_MAX_CHARS` | `600` | Max text length accepted by `/sidekick/tts` |
| `OPENAI_TTS_MODEL` | `gpt-4o-mini-tts` | OpenAI speech model |
| `OPENAI_TTS_VOICE` | `coral` | Default OpenAI voice |
| `OPENAI_TTS_FORMAT` | `wav` | Default OpenAI output format |
| `ELEVENLABS_API_KEY` | empty | Required for `SIDEKICK_STT_PROVIDER=elevenlabs` or `SIDEKICK_TTS_PROVIDER=elevenlabs` |
| `ELEVENLABS_VOICE_ID` | empty | Required ElevenLabs voice ID |
| `ELEVENLABS_TTS_MODEL` | `eleven_flash_v2_5` | ElevenLabs speech model |
| `ELEVENLABS_OUTPUT_FORMAT` | `mp3_44100_128` | ElevenLabs output format |

If `SIDEKICK_SHARED_SECRET` is set, the firmware must send either:

```text
Authorization: Bearer <secret>
```

or:

```text
X-Sidekick-Token: <secret>
```

## Device Contract

Frame request:

```http
POST /sidekick/frame?mode=hint&session=<id>
Content-Type: image/jpeg

<raw JPEG bytes>
```

Modes:

- `active`: compare against recent frames and guide when the student appears
  stuck, stopped, or incorrect.
- `hint`: stay quiet unless the student appears stuck or moving in the wrong
  direction.
- `summary`: not accepted on frame requests; summaries are generated when the
  session ends.

Response:

```json
{
  "mode": "hint",
  "session_id": "demo",
  "provider": "openai",
  "message": "Check the first visible step before simplifying.",
  "should_respond": true,
  "received_bytes": 12345,
  "latency_ms": 3
}
```

If no intervention is needed, the model returns `NO_ACTION` internally and the
API response has an empty message:

```json
{
  "mode": "hint",
  "session_id": "demo",
  "provider": "openai",
  "message": "",
  "should_respond": false,
  "received_bytes": 12345,
  "latency_ms": 3
}
```

Microphone request:

```http
POST /sidekick/audio?session=<id>&sample_rate=16000&channels=1&bits=16
Content-Type: audio/L16

<raw little-endian PCM bytes>
```

By default, the backend sends the raw PCM to ElevenLabs Scribe
(`SIDEKICK_STT_PROVIDER=elevenlabs`), stores the transcript on the session, and
attaches it to the next frame analysis. During an active device session, the
firmware uploads microphone audio about every two seconds, and also immediately
before each frame upload. Non-empty transcript chunks are appended into one
pending utterance; silence keeps the pending utterance available until the next
successful frame analysis consumes it. The alternate OpenAI provider wraps the
same PCM bytes as WAV before transcription.

Automatic device microphone uploads are disabled in the hackathon build. Set
`SIDEKICK_ENABLE_MIC_TRANSCRIPTION` to `1` in
`apps/sidekick/include/sidekick_config.h` to enable this retained experimental
path.

```json
{
  "session_id": "demo",
  "provider": "elevenlabs",
  "transcript": "Can you explain the next step?",
  "received_bytes": 64000,
  "latency_ms": 412
}
```

Session end request:

```http
POST /sidekick/session/end?session=<id>
```

This generates a summary from the retained session frames, returns a final
`summary` response, and clears the in-memory session context.

## Text To Speech

TTS is separate from image analysis. The device should call it only when a frame
or summary response has `should_respond: true` and a non-empty `message`.

OpenAI TTS:

```bash
cd apps/sidekick/backend
OPENAI_API_KEY='sk-...' SIDEKICK_TTS_PROVIDER=openai go run .
```

ElevenLabs TTS:

```bash
cd apps/sidekick/backend
ELEVENLABS_API_KEY='...' ELEVENLABS_VOICE_ID='...' SIDEKICK_TTS_PROVIDER=elevenlabs go run .
```

TTS request:

```http
POST /sidekick/tts
Content-Type: application/json

{
  "text": "Check the first visible step before simplifying."
}
```

The response body is raw audio. Useful headers:

```text
Content-Type: audio/wav
X-Sidekick-TTS-Provider: openai
X-Sidekick-Audio-Format: wav
```

Per-request overrides are supported:

```json
{
  "text": "Try the next step.",
  "provider": "elevenlabs",
  "voice": "VOICE_ID",
  "format": "mp3_44100_128"
}
```

For embedded playback, OpenAI `wav` or `pcm` is usually easier than MP3 because
the device avoids MP3 decoding. ElevenLabs defaults to MP3 unless
`ELEVENLABS_OUTPUT_FORMAT` or the request `format` is changed.
