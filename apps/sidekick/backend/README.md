# SideKick Backend

Small local backend for SideKick AI experiments.

The firmware will POST JPEG frames over Wi-Fi to this service. The service calls
local Ollama for visual tutoring, compares each frame with recent prior frames,
and returns compact JSON for the device UI.

## Run

Ollama mode, using your local `gemma4:26b`:

```bash
ollama pull gemma4:26b
cd apps/sidekick/backend
go run .
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

Optional environment variables:

| Name | Default | Description |
| --- | --- | --- |
| `PORT` | `8787` | HTTP listen port |
| `SIDEKICK_AI_PROVIDER` | `ollama` | `ollama` or `fake` |
| `OLLAMA_MODEL` | `gemma4:26b` | Local Ollama vision model |
| `OLLAMA_CHAT_URL` | `http://localhost:11434/api/chat` | Ollama chat endpoint |
| `SIDEKICK_MAX_IMAGE_BYTES` | `4194304` | Max uploaded image size |
| `SIDEKICK_SHARED_SECRET` | empty | Optional bearer token required from firmware |
| `SIDEKICK_TIMEOUT_SECONDS` | `120` | Upstream Ollama request timeout |
| `SIDEKICK_MAX_OUTPUT_TOKENS` | `240` | Max tutor response tokens |
| `SIDEKICK_CONTEXT_FRAMES` | `3` | Sliding window of recent frames retained per session |

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
