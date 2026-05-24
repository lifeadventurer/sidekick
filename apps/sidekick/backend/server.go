package main

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"
)

type server struct {
	cfg      config
	client   *http.Client
	mu       sync.Mutex
	sessions map[string]*sessionState
	ttsCache map[string]ttsCacheEntry
}

type sidekickResponse struct {
	Mode          string `json:"mode"`
	SessionID     string `json:"session_id,omitempty"`
	Provider      string `json:"provider"`
	Message       string `json:"message"`
	ShouldRespond bool   `json:"should_respond"`
	ReceivedBytes int    `json:"received_bytes"`
	LatencyMS     int64  `json:"latency_ms"`
	AudioBase64   string `json:"audio_base64,omitempty"`
	AudioFormat   string `json:"audio_format,omitempty"`
	Transcript    string `json:"transcript,omitempty"`
}

type errorResponse struct {
	Error string `json:"error"`
}

func newServer(cfg config) *server {
	return &server{
		cfg: cfg,
		client: &http.Client{
			Timeout: cfg.RequestTimeout,
		},
		sessions: make(map[string]*sessionState),
		ttsCache: make(map[string]ttsCacheEntry),
	}
}

func (s *server) authorized(r *http.Request) bool {
	if s.cfg.SharedSecret == "" {
		return true
	}

	auth := r.Header.Get("Authorization")
	if strings.HasPrefix(auth, "Bearer ") && strings.TrimPrefix(auth, "Bearer ") == s.cfg.SharedSecret {
		return true
	}

	return r.Header.Get("X-Sidekick-Token") == s.cfg.SharedSecret
}

func (s *server) readImage(r *http.Request) (io.ReadCloser, string, error) {
	r.Body = http.MaxBytesReader(nil, r.Body, s.cfg.MaxImageBytes)

	contentType := r.Header.Get("Content-Type")
	if s.cfg.AllowMultipart && strings.HasPrefix(contentType, "multipart/form-data") {
		file, header, err := r.FormFile("file")
		if err != nil {
			return nil, "", errors.New("multipart request must include file field named file")
		}
		mimeType := header.Header.Get("Content-Type")
		if mimeType == "" {
			mimeType = s.cfg.DefaultImageMIME
		}
		return file, mimeType, nil
	}

	mimeType := contentType
	if semicolon := strings.IndexByte(mimeType, ';'); semicolon >= 0 {
		mimeType = mimeType[:semicolon]
	}
	mimeType = strings.TrimSpace(mimeType)
	if mimeType == "" || mimeType == "application/octet-stream" {
		mimeType = s.cfg.DefaultImageMIME
	}

	return r.Body, mimeType, nil
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(value); err != nil {
		logError("failed to write JSON response", "error", err)
	}
}

func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		logInfo("HTTP Request", "method", r.Method, "path", r.URL.Path, "duration", time.Since(start).Round(time.Millisecond))
	})
}
