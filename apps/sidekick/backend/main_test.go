package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func testServer() *server {
	return &server{
		cfg: config{
			Provider:         "fake",
			OllamaModel:      defaultOllamaModel,
			OllamaChatURL:    defaultOllamaChatURL,
			MaxImageBytes:    defaultMaxImageBytes,
			RequestTimeout:   2 * time.Second,
			DefaultImageMIME: "image/jpeg",
			AllowMultipart:   true,
			MaxOutputTokens:  120,
		},
		client: &http.Client{Timeout: 2 * time.Second},
	}
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
