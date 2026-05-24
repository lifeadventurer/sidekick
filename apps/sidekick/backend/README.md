# SideKick Backend

Small local backend for SideKick AI experiments.

The firmware will POST JPEG frames over Wi-Fi to this service. The service calls
local Ollama for visual tutoring and returns compact JSON for the device UI.

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

If `SIDEKICK_SHARED_SECRET` is set, the firmware must send either:

```text
Authorization: Bearer <secret>
```

or:

```text
X-Sidekick-Token: <secret>
```

## Device Contract

Request:

```http
POST /sidekick/frame?mode=hint&session=<id>
Content-Type: image/jpeg

<raw JPEG bytes>
```

Response:

```json
{
  "mode": "hint",
  "session_id": "demo",
  "provider": "ollama",
  "message": "Check the first visible step before simplifying.",
  "received_bytes": 12345,
  "latency_ms": 3
}
```
