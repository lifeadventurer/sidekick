package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	defaultPort          = "8787"
	defaultProvider      = "ollama"
	defaultOllamaModel   = "gemma4:26b"
	defaultOllamaChatURL = "http://localhost:11434/api/chat"
	defaultMaxImageBytes = 4 * 1024 * 1024
)

type config struct {
	Port             string
	Provider         string
	OllamaModel      string
	OllamaChatURL    string
	MaxImageBytes    int64
	SharedSecret     string
	RequestTimeout   time.Duration
	MaxOutputTokens  int
	AllowMultipart   bool
	DefaultImageMIME string
}

type server struct {
	cfg    config
	client *http.Client
}

type sidekickResponse struct {
	Mode          string `json:"mode"`
	SessionID     string `json:"session_id,omitempty"`
	Provider      string `json:"provider"`
	Message       string `json:"message"`
	ReceivedBytes int    `json:"received_bytes"`
	LatencyMS     int64  `json:"latency_ms"`
}

type errorResponse struct {
	Error string `json:"error"`
}

func main() {
	cfg := loadConfig()
	srv := &server{
		cfg: cfg,
		client: &http.Client{
			Timeout: cfg.RequestTimeout,
		},
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", srv.handleHealth)
	mux.HandleFunc("POST /sidekick/frame", srv.handleFrame)

	addr := ":" + cfg.Port
	log.Printf("SideKick backend listening on %s provider=%s model=%s", addr, cfg.Provider, cfg.OllamaModel)
	log.Fatal(http.ListenAndServe(addr, logRequests(mux)))
}

func loadConfig() config {
	provider := getenv("SIDEKICK_AI_PROVIDER", defaultProvider)

	return config{
		Port:             getenv("PORT", defaultPort),
		Provider:         strings.ToLower(provider),
		OllamaModel:      getenv("OLLAMA_MODEL", defaultOllamaModel),
		OllamaChatURL:    getenv("OLLAMA_CHAT_URL", defaultOllamaChatURL),
		MaxImageBytes:    getenvInt64("SIDEKICK_MAX_IMAGE_BYTES", defaultMaxImageBytes),
		SharedSecret:     os.Getenv("SIDEKICK_SHARED_SECRET"),
		RequestTimeout:   time.Duration(getenvInt64("SIDEKICK_TIMEOUT_SECONDS", 120)) * time.Second,
		MaxOutputTokens:  int(getenvInt64("SIDEKICK_MAX_OUTPUT_TOKENS", 240)),
		AllowMultipart:   getenv("SIDEKICK_ALLOW_MULTIPART", "1") != "0",
		DefaultImageMIME: getenv("SIDEKICK_IMAGE_MIME", "image/jpeg"),
	}
}

func (s *server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":       true,
		"provider": s.cfg.Provider,
		"model":    s.cfg.OllamaModel,
	})
}

func (s *server) handleFrame(w http.ResponseWriter, r *http.Request) {
	start := time.Now()
	if !s.authorized(r) {
		writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "unauthorized"})
		return
	}

	image, mimeType, err := s.readImage(r)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: err.Error()})
		return
	}
	defer image.Close()

	imageBytes, err := io.ReadAll(image)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "failed to read image body"})
		return
	}
	if len(imageBytes) == 0 {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "empty image body"})
		return
	}

	mode := normalizeMode(r.URL.Query().Get("mode"))
	sessionID := strings.TrimSpace(r.URL.Query().Get("session"))

	message, provider, err := s.analyze(r.Context(), mode, mimeType, imageBytes)
	if err != nil {
		log.Printf("analysis failed: %v", err)
		writeJSON(w, http.StatusBadGateway, errorResponse{Error: err.Error()})
		return
	}

	writeJSON(w, http.StatusOK, sidekickResponse{
		Mode:          mode,
		SessionID:     sessionID,
		Provider:      provider,
		Message:       message,
		ReceivedBytes: len(imageBytes),
		LatencyMS:     time.Since(start).Milliseconds(),
	})
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

func (s *server) analyze(ctx context.Context, mode string, mimeType string, image []byte) (string, string, error) {
	switch s.cfg.Provider {
	case "fake", "mock", "":
		return fakeMessage(mode), "fake", nil
	case "ollama":
		message, err := s.callOllama(ctx, mode, image)
		return message, "ollama", err
	default:
		return "", s.cfg.Provider, fmt.Errorf("unsupported SIDEKICK_AI_PROVIDER %q", s.cfg.Provider)
	}
}

func (s *server) callOllama(ctx context.Context, mode string, image []byte) (string, error) {
	payload := ollamaChatRequest{
		Model:  s.cfg.OllamaModel,
		Stream: false,
		Think:  false,
		Messages: []ollamaMessage{
			{
				Role:    "system",
				Content: tutorSystemPrompt(mode),
			},
			{
				Role:    "user",
				Content: "Look at this SideKick camera frame and respond with only the tutor message.",
				Images:  []string{base64.StdEncoding.EncodeToString(image)},
			},
		},
		Options: ollamaOptions{
			Temperature: 0.2,
			NumPredict:  s.cfg.MaxOutputTokens,
		},
	}

	body, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.OllamaChatURL, bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := s.client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024))
	if err != nil {
		return "", err
	}

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("Ollama returned HTTP %d: %s", resp.StatusCode, string(respBody))
	}

	var parsed ollamaChatResponse
	if err := json.Unmarshal(respBody, &parsed); err != nil {
		return "", err
	}
	if parsed.Error != "" {
		return "", errors.New(parsed.Error)
	}

	text := cleanModelText(parsed.Message.Content)
	if text == "" {
		return "", errors.New("Ollama response did not include message content")
	}
	return text, nil
}

type ollamaChatRequest struct {
	Model    string          `json:"model"`
	Messages []ollamaMessage `json:"messages"`
	Stream   bool            `json:"stream"`
	Think    bool            `json:"think"`
	Options  ollamaOptions   `json:"options,omitempty"`
}

type ollamaMessage struct {
	Role     string   `json:"role"`
	Content  string   `json:"content"`
	Images   []string `json:"images,omitempty"`
	Thinking string   `json:"thinking,omitempty"`
}

type ollamaOptions struct {
	Temperature float64 `json:"temperature,omitempty"`
	NumPredict  int     `json:"num_predict,omitempty"`
}

type ollamaChatResponse struct {
	Message ollamaMessage `json:"message"`
	Error   string        `json:"error,omitempty"`
}

func normalizeMode(mode string) string {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "active", "hint", "summary":
		return strings.ToLower(strings.TrimSpace(mode))
	default:
		return "hint"
	}
}

func tutorSystemPrompt(mode string) string {
	base := "You are SideKick, a concise visual AI tutor looking at a live camera frame from a student's desk. Identify the likely learning task from the image. Respond with one short tutoring message suitable for a tiny device screen. Use at most 25 words. Do not explain your reasoning. Do not mention that you are analyzing an image."
	switch mode {
	case "active":
		return base + " In active mode, ask one guiding question or suggest the next step without giving the full answer."
	case "summary":
		return base + " In summary mode, summarize what the student appears to be working on and give one concrete next action."
	default:
		return base + " In hint mode, give a helpful hint without solving the entire problem."
	}
}

func fakeMessage(mode string) string {
	switch mode {
	case "active":
		return "What is the first thing you can label or simplify here?"
	case "summary":
		return "You are working through the visible problem. Check the setup, then verify the next step."
	default:
		return "Try identifying the main equation or diagram first."
	}
}

func cleanModelText(text string) string {
	text = strings.TrimSpace(text)
	for {
		start := strings.Index(text, "<|channel>thought")
		if start < 0 {
			break
		}
		end := strings.Index(text[start:], "<channel|>")
		if end < 0 {
			text = strings.TrimSpace(text[:start])
			break
		}
		text = text[:start] + text[start+end+len("<channel|>"):]
		text = strings.TrimSpace(text)
	}
	text = strings.ReplaceAll(text, "<|channel>final<|message>", "")
	text = strings.ReplaceAll(text, "<|message>", "")
	return strings.TrimSpace(text)
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(value); err != nil {
		log.Printf("failed to write JSON response: %v", err)
	}
}

func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("%s %s %s", r.Method, r.URL.Path, time.Since(start).Round(time.Millisecond))
	})
}

func getenv(name string, fallback string) string {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	return value
}

func getenvInt64(name string, fallback int64) int64 {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil || parsed <= 0 {
		return fallback
	}
	return parsed
}
