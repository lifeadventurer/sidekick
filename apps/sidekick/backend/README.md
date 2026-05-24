# SideKick Backend

Small local backend for SideKick AI experiments.

The firmware will POST JPEG frames over Wi-Fi to this service. The service calls
local Ollama for visual tutoring, compares each frame with recent prior frames,
and returns compact JSON for the device UI.

## Run

Ollama mode uses a local `sidekick-vision` model that stores the stable
SideKick system prompt in Ollama. Create it once before running the backend:

```bash
ollama pull llama3.2-vision:11b
cd apps/sidekick/backend
ollama create sidekick-vision -f Modelfile
go run .
```

The backend sends only per-snapshot mode and image context on each request. The
main tutor role is not repeated in every `/api/chat` payload.

The model is asked to return compact JSON with a spoken `message`, a
`should_respond` decision, and an `issue_summary`. The backend still accepts the
older plain-text/`NO_ACTION` style response so existing local models keep
working.

When you change `Modelfile`, recreate the model with:

```bash
ollama create sidekick-vision -f Modelfile
```

Use verbose logs when you want to see what Ollama returned and what the backend
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

For the hackathon demo, prefer the included `sidekick-vision` model over local
thinking-heavy models. Some thinking models can spend the whole token budget in
`thinking` and return an empty `message.content`, which the backend treats as a
failed analysis.

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
SIDEKICK_TTS_PROVIDER=elevenlabs
ELEVENLABS_API_KEY=your_api_key_here
ELEVENLABS_VOICE_ID=your_voice_id_here
```

Optional environment variables:

| Name | Default | Description |
| --- | --- | --- |
| `PORT` | `8787` | HTTP listen port |
| `SIDEKICK_AI_PROVIDER` | `ollama` | `ollama` or `fake` |
| `OLLAMA_MODEL` | `sidekick-vision:latest` | Local Ollama vision model created from `Modelfile` |
| `OLLAMA_CHAT_URL` | `http://localhost:11434/api/chat` | Ollama chat endpoint |
| `SIDEKICK_MAX_IMAGE_BYTES` | `4194304` | Max uploaded image size |
| `SIDEKICK_SHARED_SECRET` | empty | Optional bearer token required from firmware |
| `SIDEKICK_TIMEOUT_SECONDS` | `25` | Upstream Ollama request timeout |
| `SIDEKICK_MAX_OUTPUT_TOKENS` | `80` | Max tutor response tokens |
| `SIDEKICK_CONTEXT_FRAMES` | `1` | Sliding window of recent frames retained per session |
| `SIDEKICK_AI_FALLBACK` | `1` | Return demo-safe fallback JSON instead of HTTP 502 when Ollama fails (`0` disables) |
| `SIDEKICK_VERBOSE` | `0` | Enable verbose model and response logs (`go run . -verbose` overrides this) |
| `SIDEKICK_CAPTURE_DIR` | `captures` | Directory for incoming JPEG debug dumps (`off` to disable) |
| `SIDEKICK_RAG_MEMORY_PATH` | `data/rag_memory.jsonl` | Local JSONL file for future vector memories |
| `SIDEKICK_RAG_MAX_ENTRIES` | `500` | Maximum retained RAG memory records |
| `SIDEKICK_RAG_TOP_K` | `3` | Maximum similar prior issues to inject into the model prompt |
| `SIDEKICK_RAG_MIN_SIMILARITY` | `0.75` | Minimum cosine similarity for retrieved memories |
| `SIDEKICK_TTS_PROVIDER` | `none` | `none`, `openai`, or `elevenlabs` |
| `SIDEKICK_TTS_MAX_CHARS` | `600` | Max text length accepted by `/sidekick/tts` |
| `OPENAI_API_KEY` | empty | Required for `SIDEKICK_TTS_PROVIDER=openai` |
| `OPENAI_TTS_MODEL` | `gpt-4o-mini-tts` | OpenAI speech model |
| `OPENAI_TTS_VOICE` | `coral` | Default OpenAI voice |
| `OPENAI_TTS_FORMAT` | `wav` | Default OpenAI output format |
| `ELEVENLABS_API_KEY` | empty | Required for `SIDEKICK_TTS_PROVIDER=elevenlabs` |
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
  "provider": "ollama",
  "message": "Check the first visible step before simplifying.",
  "issue_summary": "The student may be simplifying without checking the first visible step.",
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
  "provider": "ollama",
  "message": "",
  "issue_summary": "The student appears to be making visible progress without needing a hint.",
  "should_respond": false,
  "received_bytes": 12345,
  "latency_ms": 3
}
```

Session end request:

```http
POST /sidekick/session/end?session=<id>
```

This generates a summary from the retained session frames, returns a final
`summary` response, and clears the in-memory session context.

## RAG Memory

The backend includes local RAG scaffolding for student issue memory:

- Every structured model response includes an `issue_summary`.
- Only `issue_summary` is intended to be embedded.
- Spoken responses only are eligible for storage: `should_respond: true`,
  non-empty `message`, and non-empty `issue_summary`.
- Memories are scoped by `session` id, persisted as JSONL, capped by
  `SIDEKICK_RAG_MAX_ENTRIES`, and searched with cosine similarity.
- Retrieved memories are injected as brief prior issues, not previous tutor
  replies.

Semantic search is currently disabled by default because no real embedder is
configured. Until an embedder is plugged into the backend, `issue_summary` is
returned in JSON but no RAG records are written or searched. The intended local
Ollama integration point is the current `/api/embed` endpoint.

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
