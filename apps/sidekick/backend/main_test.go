package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func testServer() *server {
	return newServer(config{
		Provider:         "fake",
		OllamaModel:      defaultOllamaModel,
		OllamaChatURL:    defaultOllamaChatURL,
		MaxImageBytes:    defaultMaxImageBytes,
		RequestTimeout:   2 * time.Second,
		DefaultImageMIME: "image/jpeg",
		AllowMultipart:   true,
		MaxOutputTokens:  120,
		ContextFrames:    2,
		TTS: ttsConfig{
			Provider:               "none",
			MaxChars:               defaultMaxTTSChars,
			OpenAIAPIKey:           "openai-test-key",
			OpenAIURL:              defaultOpenAITTSURL,
			OpenAIModel:            defaultOpenAITTSModel,
			OpenAIVoice:            defaultOpenAITTSVoice,
			OpenAIFormat:           defaultOpenAITTSFormat,
			ElevenLabsAPIKey:       "elevenlabs-test-key",
			ElevenLabsURL:          defaultElevenLabsTTSURL,
			ElevenLabsVoiceID:      "voice-test-id",
			ElevenLabsModel:        defaultElevenLabsTTSModel,
			ElevenLabsOutputFormat: defaultElevenLabsOutputFormat,
		},
	})
}

func TestHealth(t *testing.T) {
	srv := testServer()

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	rec := httptest.NewRecorder()

	srv.handleHealth(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected %d, got %d", http.StatusOK, rec.Code)
	}
}

func TestFrameFakeProvider(t *testing.T) {
	srv := testServer()
	body := []byte{0xff, 0xd8, 0xff, 0xd9}

	req := httptest.NewRequest(http.MethodPost, "/sidekick/frame?mode=active&session=test-session", bytes.NewReader(body))
	req.Header.Set("Content-Type", "image/jpeg")
	rec := httptest.NewRecorder()

	srv.handleFrame(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected %d, got %d body=%s", http.StatusOK, rec.Code, rec.Body.String())
	}

	var parsed sidekickResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.Mode != "active" {
		t.Fatalf("expected active mode, got %q", parsed.Mode)
	}
	if parsed.SessionID != "test-session" {
		t.Fatalf("expected session id, got %q", parsed.SessionID)
	}
	if parsed.Provider != "fake" {
		t.Fatalf("expected fake provider, got %q", parsed.Provider)
	}
	if parsed.ReceivedBytes != len(body) {
		t.Fatalf("expected %d bytes, got %d", len(body), parsed.ReceivedBytes)
	}
	if parsed.Message == "" {
		t.Fatal("expected non-empty message")
	}
	if !parsed.ShouldRespond {
		t.Fatal("expected fake provider to respond")
	}
}

func TestFrameOllamaProvider(t *testing.T) {
	srv := testServer()
	srv.cfg.Provider = "ollama"
	srv.cfg.OllamaChatURL = "http://ollama.test/api/chat"
	srv.client = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method != http.MethodPost {
			t.Fatalf("expected POST, got %s", r.Method)
		}
		if r.URL.String() != srv.cfg.OllamaChatURL {
			t.Fatalf("expected URL %q, got %q", srv.cfg.OllamaChatURL, r.URL.String())
		}

		var payload ollamaChatRequest
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			return nil, err
		}
		if payload.Model != defaultOllamaModel {
			t.Fatalf("expected model %q, got %q", defaultOllamaModel, payload.Model)
		}
		if payload.Stream {
			t.Fatal("expected non-streaming request")
		}
		if payload.Think {
			t.Fatal("expected think=false request")
		}
		if len(payload.Messages) != 2 {
			t.Fatalf("expected 2 messages, got %d", len(payload.Messages))
		}
		if len(payload.Messages[1].Images) != 1 {
			t.Fatalf("expected 1 image, got %d", len(payload.Messages[1].Images))
		}

		body, err := json.Marshal(ollamaChatResponse{
			Message: ollamaMessage{
				Role:    "assistant",
				Content: "Check the sign before you simplify.",
			},
		})
		if err != nil {
			return nil, err
		}
		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(bytes.NewReader(body)),
		}, nil
	})}

	body := []byte{0xff, 0xd8, 0xff, 0xd9}
	req := httptest.NewRequest(http.MethodPost, "/sidekick/frame?mode=hint", bytes.NewReader(body))
	req.Header.Set("Content-Type", "image/jpeg")
	rec := httptest.NewRecorder()

	srv.handleFrame(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected %d, got %d body=%s", http.StatusOK, rec.Code, rec.Body.String())
	}

	var parsed sidekickResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.Provider != "ollama" {
		t.Fatalf("expected ollama provider, got %q", parsed.Provider)
	}
	if parsed.Message != "Check the sign before you simplify." {
		t.Fatalf("unexpected message %q", parsed.Message)
	}
	if !parsed.ShouldRespond {
		t.Fatal("expected should_respond=true")
	}
}

func TestOllamaNoAction(t *testing.T) {
	srv := testServer()
	srv.cfg.Provider = "ollama"
	srv.cfg.OllamaChatURL = "http://ollama.test/api/chat"
	srv.client = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		body, err := json.Marshal(ollamaChatResponse{
			Message: ollamaMessage{
				Role:    "assistant",
				Content: "NO_ACTION",
			},
		})
		if err != nil {
			return nil, err
		}
		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(bytes.NewReader(body)),
		}, nil
	})}

	req := httptest.NewRequest(http.MethodPost, "/sidekick/frame?mode=hint", bytes.NewReader([]byte("image")))
	req.Header.Set("Content-Type", "image/jpeg")
	rec := httptest.NewRecorder()

	srv.handleFrame(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected %d, got %d body=%s", http.StatusOK, rec.Code, rec.Body.String())
	}

	var parsed sidekickResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.ShouldRespond {
		t.Fatal("expected should_respond=false")
	}
	if parsed.Message != "" {
		t.Fatalf("expected empty message, got %q", parsed.Message)
	}
}

func TestSlidingFrameContext(t *testing.T) {
	srv := testServer()

	for idx := 0; idx < 3; idx++ {
		req := httptest.NewRequest(http.MethodPost, "/sidekick/frame?mode=active&session=abc", bytes.NewReader([]byte{byte(idx + 1)}))
		req.Header.Set("Content-Type", "image/jpeg")
		rec := httptest.NewRecorder()

		srv.handleFrame(rec, req)

		if rec.Code != http.StatusOK {
			t.Fatalf("frame %d expected %d, got %d", idx, http.StatusOK, rec.Code)
		}
	}

	frames := srv.sessionFrames("abc")
	if len(frames) != 2 {
		t.Fatalf("expected 2 retained frames, got %d", len(frames))
	}
	if frames[0].Image[0] != 2 || frames[1].Image[0] != 3 {
		t.Fatalf("expected last two frames, got %v and %v", frames[0].Image, frames[1].Image)
	}
}

func TestOllamaUsesPriorFrameContext(t *testing.T) {
	srv := testServer()
	srv.cfg.Provider = "ollama"
	srv.cfg.OllamaChatURL = "http://ollama.test/api/chat"

	requestCount := 0
	srv.client = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		requestCount++
		var payload ollamaChatRequest
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			return nil, err
		}
		if requestCount == 2 {
			if len(payload.Messages) != 3 {
				t.Fatalf("expected system, previous frame, current frame messages; got %d", len(payload.Messages))
			}
			if len(payload.Messages[1].Images) != 1 || len(payload.Messages[2].Images) != 1 {
				t.Fatalf("expected previous and current images in second request")
			}
		}

		body, err := json.Marshal(ollamaChatResponse{
			Message: ollamaMessage{Role: "assistant", Content: "Keep checking the next step."},
		})
		if err != nil {
			return nil, err
		}
		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(bytes.NewReader(body)),
		}, nil
	})}

	for idx := 0; idx < 2; idx++ {
		req := httptest.NewRequest(http.MethodPost, "/sidekick/frame?mode=active&session=abc", bytes.NewReader([]byte{byte(idx + 1)}))
		req.Header.Set("Content-Type", "image/jpeg")
		rec := httptest.NewRecorder()

		srv.handleFrame(rec, req)

		if rec.Code != http.StatusOK {
			t.Fatalf("frame %d expected %d, got %d body=%s", idx, http.StatusOK, rec.Code, rec.Body.String())
		}
	}
}

func TestSessionEndSummary(t *testing.T) {
	srv := testServer()

	req := httptest.NewRequest(http.MethodPost, "/sidekick/frame?mode=hint&session=abc", bytes.NewReader([]byte("image")))
	req.Header.Set("Content-Type", "image/jpeg")
	rec := httptest.NewRecorder()
	srv.handleFrame(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("frame expected %d, got %d", http.StatusOK, rec.Code)
	}

	endReq := httptest.NewRequest(http.MethodPost, "/sidekick/session/end?session=abc", nil)
	endRec := httptest.NewRecorder()
	srv.handleSessionEnd(endRec, endReq)
	if endRec.Code != http.StatusOK {
		t.Fatalf("summary expected %d, got %d body=%s", http.StatusOK, endRec.Code, endRec.Body.String())
	}

	var parsed sidekickResponse
	if err := json.Unmarshal(endRec.Body.Bytes(), &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.Mode != "summary" {
		t.Fatalf("expected summary mode, got %q", parsed.Mode)
	}
	if parsed.Message == "" {
		t.Fatal("expected summary message")
	}
	if len(srv.sessionFrames("abc")) != 0 {
		t.Fatal("expected session to be cleared after summary")
	}
}

func TestSummaryModeRejectedForFrame(t *testing.T) {
	srv := testServer()

	req := httptest.NewRequest(http.MethodPost, "/sidekick/frame?mode=summary", bytes.NewReader([]byte("image")))
	req.Header.Set("Content-Type", "image/jpeg")
	rec := httptest.NewRecorder()
	srv.handleFrame(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Fatalf("expected %d, got %d", http.StatusBadRequest, rec.Code)
	}
}

func TestTTSDisabled(t *testing.T) {
	srv := testServer()
	req := httptest.NewRequest(http.MethodPost, "/sidekick/tts", bytes.NewReader([]byte(`{"text":"hello"}`)))
	rec := httptest.NewRecorder()

	srv.handleTTS(rec, req)

	if rec.Code != http.StatusBadGateway {
		t.Fatalf("expected %d, got %d", http.StatusBadGateway, rec.Code)
	}
}

func TestOpenAITTS(t *testing.T) {
	srv := testServer()
	srv.cfg.TTS.Provider = "openai"
	srv.cfg.TTS.OpenAIURL = "https://openai.test/v1/audio/speech"
	srv.client = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method != http.MethodPost {
			t.Fatalf("expected POST, got %s", r.Method)
		}
		if r.URL.String() != srv.cfg.TTS.OpenAIURL {
			t.Fatalf("expected URL %q, got %q", srv.cfg.TTS.OpenAIURL, r.URL.String())
		}
		if r.Header.Get("Authorization") != "Bearer openai-test-key" {
			t.Fatalf("missing OpenAI authorization header")
		}

		var payload map[string]string
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			return nil, err
		}
		if payload["model"] != defaultOpenAITTSModel {
			t.Fatalf("unexpected OpenAI model %q", payload["model"])
		}
		if payload["voice"] != "alloy" {
			t.Fatalf("unexpected OpenAI voice %q", payload["voice"])
		}
		if payload["response_format"] != "pcm" {
			t.Fatalf("unexpected OpenAI format %q", payload["response_format"])
		}
		if payload["input"] != "Read this" {
			t.Fatalf("unexpected input %q", payload["input"])
		}

		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     http.Header{"Content-Type": []string{"audio/L16"}},
			Body:       io.NopCloser(bytes.NewReader([]byte("audio"))),
		}, nil
	})}

	req := httptest.NewRequest(http.MethodPost, "/sidekick/tts", bytes.NewReader([]byte(`{"text":"Read this","voice":"alloy","format":"pcm"}`)))
	rec := httptest.NewRecorder()

	srv.handleTTS(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected %d, got %d body=%s", http.StatusOK, rec.Code, rec.Body.String())
	}
	if rec.Header().Get("X-Sidekick-TTS-Provider") != "openai" {
		t.Fatalf("unexpected provider header %q", rec.Header().Get("X-Sidekick-TTS-Provider"))
	}
	if rec.Header().Get("X-Sidekick-Audio-Format") != "pcm" {
		t.Fatalf("unexpected format header %q", rec.Header().Get("X-Sidekick-Audio-Format"))
	}
	if rec.Body.String() != "audio" {
		t.Fatalf("unexpected audio body %q", rec.Body.String())
	}
}

func TestElevenLabsTTS(t *testing.T) {
	srv := testServer()
	srv.cfg.TTS.Provider = "elevenlabs"
	srv.cfg.TTS.ElevenLabsURL = "https://api.elevenlabs.test/v1/text-to-speech"
	srv.client = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method != http.MethodPost {
			t.Fatalf("expected POST, got %s", r.Method)
		}
		expectedURL := "https://api.elevenlabs.test/v1/text-to-speech/voice-test-id?output_format=mp3_44100_128"
		if r.URL.String() != expectedURL {
			t.Fatalf("expected URL %q, got %q", expectedURL, r.URL.String())
		}
		if r.Header.Get("xi-api-key") != "elevenlabs-test-key" {
			t.Fatalf("missing ElevenLabs API key header")
		}

		var payload map[string]string
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			return nil, err
		}
		if payload["model_id"] != defaultElevenLabsTTSModel {
			t.Fatalf("unexpected ElevenLabs model %q", payload["model_id"])
		}
		if payload["text"] != "Speak this" {
			t.Fatalf("unexpected text %q", payload["text"])
		}

		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     http.Header{"Content-Type": []string{"audio/mpeg"}},
			Body:       io.NopCloser(bytes.NewReader([]byte("mp3"))),
		}, nil
	})}

	req := httptest.NewRequest(http.MethodPost, "/sidekick/tts", bytes.NewReader([]byte(`{"text":"Speak this"}`)))
	rec := httptest.NewRecorder()

	srv.handleTTS(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected %d, got %d body=%s", http.StatusOK, rec.Code, rec.Body.String())
	}
	if rec.Header().Get("X-Sidekick-TTS-Provider") != "elevenlabs" {
		t.Fatalf("unexpected provider header %q", rec.Header().Get("X-Sidekick-TTS-Provider"))
	}
	if rec.Header().Get("X-Sidekick-Audio-Format") != defaultElevenLabsOutputFormat {
		t.Fatalf("unexpected format header %q", rec.Header().Get("X-Sidekick-Audio-Format"))
	}
	if rec.Body.String() != "mp3" {
		t.Fatalf("unexpected audio body %q", rec.Body.String())
	}
}

func TestTTSMaxChars(t *testing.T) {
	srv := testServer()
	srv.cfg.TTS.Provider = "openai"
	srv.cfg.TTS.MaxChars = 3

	req := httptest.NewRequest(http.MethodPost, "/sidekick/tts", bytes.NewReader([]byte(`{"text":"too long"}`)))
	rec := httptest.NewRecorder()

	srv.handleTTS(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Fatalf("expected %d, got %d", http.StatusBadRequest, rec.Code)
	}
}

func TestLoadEnvFile(t *testing.T) {
	dir := t.TempDir()
	envPath := filepath.Join(dir, ".env.local")
	if err := os.WriteFile(envPath, []byte(`
# comment
SIDEKICK_TEST_ENV=loaded
export SIDEKICK_TEST_QUOTED="quoted value"
SIDEKICK_TEST_SINGLE='single value'
`), 0o600); err != nil {
		t.Fatal(err)
	}

	t.Setenv("SIDEKICK_TEST_ENV", "")
	t.Setenv("SIDEKICK_TEST_QUOTED", "")
	t.Setenv("SIDEKICK_TEST_SINGLE", "")

	if err := loadEnvFile(envPath); err != nil {
		t.Fatal(err)
	}
	if os.Getenv("SIDEKICK_TEST_ENV") != "loaded" {
		t.Fatalf("expected loaded env, got %q", os.Getenv("SIDEKICK_TEST_ENV"))
	}
	if os.Getenv("SIDEKICK_TEST_QUOTED") != "quoted value" {
		t.Fatalf("expected quoted value, got %q", os.Getenv("SIDEKICK_TEST_QUOTED"))
	}
	if os.Getenv("SIDEKICK_TEST_SINGLE") != "single value" {
		t.Fatalf("expected single value, got %q", os.Getenv("SIDEKICK_TEST_SINGLE"))
	}
}

func TestLoadEnvFileDoesNotOverrideShellEnv(t *testing.T) {
	dir := t.TempDir()
	envPath := filepath.Join(dir, ".env.local")
	if err := os.WriteFile(envPath, []byte("SIDEKICK_TEST_KEEP=file\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("SIDEKICK_TEST_KEEP", "shell")

	if err := loadEnvFile(envPath); err != nil {
		t.Fatal(err)
	}
	if os.Getenv("SIDEKICK_TEST_KEEP") != "shell" {
		t.Fatalf("expected shell env to win, got %q", os.Getenv("SIDEKICK_TEST_KEEP"))
	}
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) {
	return f(r)
}

func TestSharedSecret(t *testing.T) {
	srv := testServer()
	srv.cfg.SharedSecret = "secret"

	req := httptest.NewRequest(http.MethodPost, "/sidekick/frame", bytes.NewReader([]byte("image")))
	rec := httptest.NewRecorder()

	srv.handleFrame(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected %d, got %d", http.StatusUnauthorized, rec.Code)
	}
}
