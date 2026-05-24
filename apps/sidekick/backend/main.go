package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	defaultPort            = "8787"
	defaultProvider        = "ollama"
	defaultOllamaModel     = "sidekick-vision:latest"
	defaultOllamaChatURL   = "http://localhost:11434/api/chat"
	defaultMaxImageBytes   = 4 * 1024 * 1024
	defaultMaxOutputTokens = 80
	defaultContextFrames   = 1
	defaultRequestTimeout  = 25
	defaultSessionID       = "default"
	defaultTTSProvider     = "none"
	defaultMaxTTSChars     = 600
	defaultCaptureDir      = "captures"
	defaultAudioCacheDir   = "audio_cache"

	defaultOpenAITTSURL    = "https://api.openai.com/v1/audio/speech"
	defaultOpenAITTSModel  = "gpt-4o-mini-tts"
	defaultOpenAITTSVoice  = "coral"
	defaultOpenAITTSFormat = "wav"

	defaultElevenLabsTTSURL       = "https://api.elevenlabs.io/v1/text-to-speech"
	defaultElevenLabsTTSModel     = "eleven_flash_v2_5"
	defaultElevenLabsOutputFormat = "mp3_44100_128"
)

type config struct {
	Port              string
	Provider          string
	OllamaModel       string
	OllamaChatURL     string
	MaxImageBytes     int64
	SharedSecret      string
	RequestTimeout    time.Duration
	MaxOutputTokens   int
	ContextFrames     int
	FallbackOnAIError bool
	Verbose           bool
	AllowMultipart    bool
	DefaultImageMIME  string
	CaptureDir        string
	TTS               ttsConfig
}

type ttsConfig struct {
	Provider               string
	MaxChars               int
	AudioCacheDir          string
	OpenAIAPIKey           string
	OpenAIURL              string
	OpenAIModel            string
	OpenAIVoice            string
	OpenAIFormat           string
	ElevenLabsAPIKey       string
	ElevenLabsURL          string
	ElevenLabsVoiceID      string
	ElevenLabsModel        string
	ElevenLabsOutputFormat string
}

type ttsCacheEntry struct {
	Audio       []byte
	ContentType string
	Format      string
	CreatedAt   time.Time
}

type server struct {
	cfg      config
	client   *http.Client
	mu       sync.Mutex
	sessions map[string]*sessionState
	ttsCache map[string]ttsCacheEntry
}

type sessionState struct {
	Frames []frameContext
}

type frameContext struct {
	Image         []byte
	Mode          string
	Message       string
	ShouldRespond bool
	ObservedAt    time.Time
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
}

type errorResponse struct {
	Error string `json:"error"`
}

type ttsRequest struct {
	Text     string `json:"text"`
	Provider string `json:"provider,omitempty"`
	Voice    string `json:"voice,omitempty"`
	Format   string `json:"format,omitempty"`
}

func main() {
	log.SetFlags(0)
	loadLocalEnv()
	cfg := loadConfig()
	flag.BoolVar(&cfg.Verbose, "verbose", cfg.Verbose, "print verbose Ollama and response logs")
	flag.Parse()
	srv := newServer(cfg)
	srv.loadTTSFromDisk()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", srv.handleHealth)
	mux.HandleFunc("POST /sidekick/frame", srv.handleFrame)
	mux.HandleFunc("POST /sidekick/session/end", srv.handleSessionEnd)
	mux.HandleFunc("POST /sidekick/tts", srv.handleTTS)

	addr := ":" + cfg.Port
	logInfo("SideKick backend listening", "addr", addr, "provider", cfg.Provider, "model", cfg.OllamaModel, "verbose", cfg.Verbose)
	log.Fatal(http.ListenAndServe(addr, logRequests(mux)))
}

func loadConfig() config {
	provider := getenv("SIDEKICK_AI_PROVIDER", defaultProvider)

	return config{
		Port:              getenv("PORT", defaultPort),
		Provider:          strings.ToLower(provider),
		OllamaModel:       getenv("OLLAMA_MODEL", defaultOllamaModel),
		OllamaChatURL:     getenv("OLLAMA_CHAT_URL", defaultOllamaChatURL),
		MaxImageBytes:     getenvInt64("SIDEKICK_MAX_IMAGE_BYTES", defaultMaxImageBytes),
		SharedSecret:      os.Getenv("SIDEKICK_SHARED_SECRET"),
		RequestTimeout:    time.Duration(getenvInt64("SIDEKICK_TIMEOUT_SECONDS", defaultRequestTimeout)) * time.Second,
		MaxOutputTokens:   int(getenvInt64("SIDEKICK_MAX_OUTPUT_TOKENS", defaultMaxOutputTokens)),
		ContextFrames:     int(getenvInt64("SIDEKICK_CONTEXT_FRAMES", defaultContextFrames)),
		FallbackOnAIError: getenvBool("SIDEKICK_AI_FALLBACK", true),
		Verbose:           getenvBool("SIDEKICK_VERBOSE", false),
		AllowMultipart:    getenv("SIDEKICK_ALLOW_MULTIPART", "1") != "0",
		DefaultImageMIME:  getenv("SIDEKICK_IMAGE_MIME", "image/jpeg"),
		CaptureDir:        loadCaptureDir(),
		TTS: ttsConfig{
			Provider:               strings.ToLower(getenv("SIDEKICK_TTS_PROVIDER", defaultTTSProvider)),
			MaxChars:               int(getenvInt64("SIDEKICK_TTS_MAX_CHARS", defaultMaxTTSChars)),
			AudioCacheDir:          getenv("SIDEKICK_AUDIO_CACHE_DIR", defaultAudioCacheDir),
			OpenAIAPIKey:           os.Getenv("OPENAI_API_KEY"),
			OpenAIURL:              getenv("OPENAI_TTS_URL", defaultOpenAITTSURL),
			OpenAIModel:            getenv("OPENAI_TTS_MODEL", defaultOpenAITTSModel),
			OpenAIVoice:            getenv("OPENAI_TTS_VOICE", defaultOpenAITTSVoice),
			OpenAIFormat:           getenv("OPENAI_TTS_FORMAT", defaultOpenAITTSFormat),
			ElevenLabsAPIKey:       os.Getenv("ELEVENLABS_API_KEY"),
			ElevenLabsURL:          getenv("ELEVENLABS_TTS_URL", defaultElevenLabsTTSURL),
			ElevenLabsVoiceID:      os.Getenv("ELEVENLABS_VOICE_ID"),
			ElevenLabsModel:        getenv("ELEVENLABS_TTS_MODEL", defaultElevenLabsTTSModel),
			ElevenLabsOutputFormat: getenv("ELEVENLABS_OUTPUT_FORMAT", defaultElevenLabsOutputFormat),
		},
	}
}

func loadLocalEnv() {
	for _, path := range []string{".env.local", ".env"} {
		if err := loadEnvFile(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			logWarn("failed to load env file", "path", path, "error", err)
		}
	}
}

func loadEnvFile(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}

	lines := strings.Split(string(data), "\n")
	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		line = strings.TrimPrefix(line, "export ")

		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)
		if key == "" || os.Getenv(key) != "" {
			continue
		}
		value = strings.Trim(value, "\"'")
		if err := os.Setenv(key, value); err != nil {
			return err
		}
	}
	return nil
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

func (s *server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":       true,
		"provider": s.cfg.Provider,
		"model":    s.cfg.OllamaModel,
		"context":  s.cfg.ContextFrames,
		"tts":      s.cfg.TTS.Provider,
	})
}

func (s *server) handleFrame(w http.ResponseWriter, r *http.Request) {
	start := time.Now()
	if !s.authorized(r) {
		writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "unauthorized"})
		return
	}

	image, _, err := s.readImage(r)
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

	sessionID := normalizeSessionID(r.URL.Query().Get("session"))
	mode := normalizeMode(r.URL.Query().Get("mode"))
	s.persistCapture(sessionID, mode, imageBytes)
	if mode == "summary" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "summary is generated by POST /sidekick/session/end"})
		return
	}
	priorFrames := s.sessionFrames(sessionID)

	message, shouldRespond, provider, err := s.analyze(r.Context(), mode, imageBytes, priorFrames, false)
	if err != nil {
		logError("analysis failed", "error", err)
		writeJSON(w, http.StatusBadGateway, errorResponse{Error: err.Error()})
		return
	}
	s.recordFrame(sessionID, frameContext{
		Image:         append([]byte(nil), imageBytes...),
		Mode:          mode,
		Message:       message,
		ShouldRespond: shouldRespond,
		ObservedAt:    time.Now(),
	})

	if s.cfg.Verbose {
		logInfo("frame result", "session", sessionID, "provider", provider, "mode", mode, "should_respond", shouldRespond, "message", message)
	}

	s.maybeGenerateTTS(r.Context(), shouldRespond, message)

	writeJSON(w, http.StatusOK, sidekickResponse{
		Mode:          mode,
		SessionID:     sessionID,
		Provider:      provider,
		Message:       message,
		ShouldRespond: shouldRespond,
		ReceivedBytes: len(imageBytes),
		LatencyMS:     time.Since(start).Milliseconds(),
	})
}

func (s *server) handleSessionEnd(w http.ResponseWriter, r *http.Request) {
	start := time.Now()
	if !s.authorized(r) {
		writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "unauthorized"})
		return
	}

	sessionID := normalizeSessionID(r.URL.Query().Get("session"))
	frames := s.sessionFrames(sessionID)
	if len(frames) == 0 {
		writeJSON(w, http.StatusNotFound, errorResponse{Error: "session has no captured frames"})
		return
	}

	message, shouldRespond, provider, err := s.analyze(r.Context(), "summary", nil, frames, true)
	if err != nil {
		logError("summary failed", "error", err)
		writeJSON(w, http.StatusBadGateway, errorResponse{Error: err.Error()})
		return
	}
	s.clearSession(sessionID)

	if s.cfg.Verbose {
		logInfo("summary result", "session", sessionID, "provider", provider, "should_respond", shouldRespond, "message", message)
	}

	s.maybeGenerateTTS(r.Context(), shouldRespond, message)

	writeJSON(w, http.StatusOK, sidekickResponse{
		Mode:          "summary",
		SessionID:     sessionID,
		Provider:      provider,
		Message:       message,
		ShouldRespond: shouldRespond,
		LatencyMS:     time.Since(start).Milliseconds(),
	})
}

func (s *server) handleTTS(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeJSON(w, http.StatusUnauthorized, errorResponse{Error: "unauthorized"})
		return
	}

	var req ttsRequest
	r.Body = http.MaxBytesReader(w, r.Body, int64(s.cfg.TTS.MaxChars*4+1024))
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "invalid JSON body"})
		return
	}

	text := strings.TrimSpace(req.Text)
	if text == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "text is required"})
		return
	}
	if len([]rune(text)) > s.cfg.TTS.MaxChars {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: fmt.Sprintf("text exceeds %d characters", s.cfg.TTS.MaxChars)})
		return
	}

	provider := strings.ToLower(strings.TrimSpace(req.Provider))
	if provider == "" {
		provider = s.cfg.TTS.Provider
	}

	key := normalizeTextKey(text)
	var entry ttsCacheEntry
	var found bool

	if entry, found = s.getCachedTTS(key); !found {
		// Try prefix match on cached keys (in case of client truncation)
		s.mu.Lock()
		for cachedKey, cachedEntry := range s.ttsCache {
			if len(key) >= 10 && strings.HasPrefix(cachedKey, key) {
				entry = cachedEntry
				found = true
				if s.cfg.Verbose {
					logInfo("TTS cache prefix match hit", "requested", key, "matched", cachedKey)
				}
				break
			}
		}
		s.mu.Unlock()
	}

	if found {
		if s.cfg.Verbose {
			logInfo("TTS cache hit", "key", key, "bytes", len(entry.Audio))
		}
		w.Header().Set("Content-Type", entry.ContentType)
		w.Header().Set("Content-Length", strconv.Itoa(len(entry.Audio)))
		w.Header().Set("X-Sidekick-TTS-Provider", provider+"-cached")
		w.Header().Set("X-Sidekick-Audio-Format", entry.Format)
		w.WriteHeader(http.StatusOK)
		if _, err := w.Write(entry.Audio); err != nil {
			logError("failed to write TTS audio", "error", err)
		}
		return
	}

	audio, contentType, format, err := s.synthesizeSpeech(r.Context(), provider, req)
	if err != nil {
		writeJSON(w, http.StatusBadGateway, errorResponse{Error: err.Error()})
		return
	}

	s.cacheTTS(key, audio, contentType, format, text)

	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Content-Length", strconv.Itoa(len(audio)))
	w.Header().Set("X-Sidekick-TTS-Provider", provider)
	w.Header().Set("X-Sidekick-Audio-Format", format)
	w.WriteHeader(http.StatusOK)
	if _, err := w.Write(audio); err != nil {
		logError("failed to write TTS audio", "error", err)
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

func normalizeTextKey(text string) string {
	return strings.ToLower(strings.Join(strings.Fields(text), " "))
}

func (s *server) cacheTTS(key string, audio []byte, contentType, format string, text string) {
	s.mu.Lock()
	now := time.Now()
	for k, v := range s.ttsCache {
		if now.Sub(v.CreatedAt) > 10*time.Minute {
			delete(s.ttsCache, k)
		}
	}

	if len(s.ttsCache) > 50 {
		s.ttsCache = make(map[string]ttsCacheEntry)
	}

	entry := ttsCacheEntry{
		Audio:       audio,
		ContentType: contentType,
		Format:      format,
		CreatedAt:   now,
	}
	s.ttsCache[key] = entry
	s.mu.Unlock()

	s.saveTTSToDisk(key, entry, text)
}

func (s *server) getCachedTTS(key string) (ttsCacheEntry, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	entry, found := s.ttsCache[key]
	return entry, found
}

type ttsMetadata struct {
	Key         string    `json:"key"`
	Text        string    `json:"text"`
	Format      string    `json:"format"`
	ContentType string    `json:"content_type"`
	CreatedAt   time.Time `json:"created_at"`
	AudioFile   string    `json:"audio_file"`
}

func extensionForFormat(format string) string {
	format = strings.ToLower(format)
	switch {
	case strings.Contains(format, "wav"):
		return ".wav"
	case strings.Contains(format, "pcm"):
		return ".pcm"
	case strings.Contains(format, "opus"):
		return ".opus"
	case strings.Contains(format, "aac"):
		return ".aac"
	case strings.Contains(format, "flac"):
		return ".flac"
	case strings.Contains(format, "mp3"):
		return ".mp3"
	default:
		return ".bin"
	}
}

func (s *server) saveTTSToDisk(key string, entry ttsCacheEntry, text string) {
	if s.cfg.TTS.AudioCacheDir == "" || len(entry.Audio) == 0 {
		return
	}

	if err := os.MkdirAll(s.cfg.TTS.AudioCacheDir, 0o755); err != nil {
		logError("audio cache save mkdir failed", "error", err)
		return
	}

	hash := fmt.Sprintf("%x", sha256.Sum256([]byte(key)))

	// 1. Save raw text file
	txtPath := filepath.Join(s.cfg.TTS.AudioCacheDir, hash+".txt")
	if err := os.WriteFile(txtPath, []byte(text), 0o644); err != nil {
		logError("failed to write TTS text file", "path", txtPath, "error", err)
	}

	// 2. Save raw audio file
	ext := extensionForFormat(entry.Format)
	audioFilename := hash + ext
	audioPath := filepath.Join(s.cfg.TTS.AudioCacheDir, audioFilename)
	if err := os.WriteFile(audioPath, entry.Audio, 0o644); err != nil {
		logError("failed to write TTS audio file", "path", audioPath, "error", err)
	}

	// 3. Save JSON metadata
	meta := ttsMetadata{
		Key:         key,
		Text:        text,
		Format:      entry.Format,
		ContentType: entry.ContentType,
		CreatedAt:   entry.CreatedAt,
		AudioFile:   audioFilename,
	}
	metaBytes, err := json.MarshalIndent(meta, "", "  ")
	if err != nil {
		logError("failed to marshal TTS metadata", "error", err)
		return
	}
	metaPath := filepath.Join(s.cfg.TTS.AudioCacheDir, hash+".json")
	if err := os.WriteFile(metaPath, metaBytes, 0o644); err != nil {
		logError("failed to write TTS metadata file", "path", metaPath, "error", err)
	}
}

func (s *server) loadTTSFromDisk() {
	if s.cfg.TTS.AudioCacheDir == "" {
		return
	}

	// Check if directory exists
	if _, err := os.Stat(s.cfg.TTS.AudioCacheDir); os.IsNotExist(err) {
		return
	}

	files, err := os.ReadDir(s.cfg.TTS.AudioCacheDir)
	if err != nil {
		logError("failed to read audio cache directory", "error", err)
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	loadedCount := 0
	for _, file := range files {
		if file.IsDir() || !strings.HasSuffix(file.Name(), ".json") {
			continue
		}

		// Read and parse json metadata
		metaPath := filepath.Join(s.cfg.TTS.AudioCacheDir, file.Name())
		metaBytes, err := os.ReadFile(metaPath)
		if err != nil {
			logWarn("failed to read metadata file", "path", metaPath, "error", err)
			continue
		}

		var meta ttsMetadata
		if err := json.Unmarshal(metaBytes, &meta); err != nil {
			logWarn("failed to parse metadata file", "path", metaPath, "error", err)
			continue
		}

		if meta.Key == "" || meta.AudioFile == "" {
			continue
		}

		// Read the associated audio file
		audioPath := filepath.Join(s.cfg.TTS.AudioCacheDir, meta.AudioFile)
		audioBytes, err := os.ReadFile(audioPath)
		if err != nil {
			logWarn("failed to read audio file for metadata", "path", audioPath, "error", err)
			continue
		}

		s.ttsCache[meta.Key] = ttsCacheEntry{
			Audio:       audioBytes,
			ContentType: meta.ContentType,
			Format:      meta.Format,
			CreatedAt:   meta.CreatedAt,
		}
		loadedCount++
	}

	if loadedCount > 0 {
		logInfo("loaded TTS cache from disk", "count", loadedCount, "dir", s.cfg.TTS.AudioCacheDir)
	}
}

func (s *server) maybeGenerateTTS(_ context.Context, shouldRespond bool, message string) {
	if !shouldRespond || message == "" {
		return
	}
	provider := s.cfg.TTS.Provider
	if provider == "" || provider == "none" {
		return
	}

	// Use a background context so TTS completes even if the board HTTP client
	// has already disconnected (same pattern as callOllama).
	ttsCtx, cancel := context.WithTimeout(context.Background(), s.cfg.RequestTimeout)
	defer cancel()

	format := "pcm_16000"
	start := time.Now()
	audio, contentType, outputFormat, err := s.synthesizeSpeech(ttsCtx, provider, ttsRequest{
		Text:   message,
		Format: format,
	})
	if err != nil {
		logWarn("pre-generation TTS failed (non-fatal)", "error", err)
		return
	}

	key := normalizeTextKey(message)
	s.cacheTTS(key, audio, contentType, outputFormat, message)

	if s.cfg.Verbose {
		logInfo("pre-generated and cached TTS", "provider", provider, "latency", time.Since(start).Round(time.Millisecond), "audio_bytes", len(audio), "key", key)
	}
}

func (s *server) synthesizeSpeech(ctx context.Context, provider string, req ttsRequest) ([]byte, string, string, error) {
	switch provider {
	case "", "none":
		return nil, "", "", errors.New("TTS provider is disabled; set SIDEKICK_TTS_PROVIDER=openai or elevenlabs")
	case "openai":
		return s.callOpenAITTS(ctx, req)
	case "elevenlabs", "eleven":
		return s.callElevenLabsTTS(ctx, req)
	default:
		return nil, "", "", fmt.Errorf("unsupported SIDEKICK_TTS_PROVIDER %q", provider)
	}
}

func (s *server) callOpenAITTS(ctx context.Context, req ttsRequest) ([]byte, string, string, error) {
	if s.cfg.TTS.OpenAIAPIKey == "" {
		return nil, "", "", errors.New("OPENAI_API_KEY is required for OpenAI TTS")
	}

	voice := firstNonEmpty(req.Voice, s.cfg.TTS.OpenAIVoice)
	format := firstNonEmpty(req.Format, s.cfg.TTS.OpenAIFormat)
	body, err := json.Marshal(map[string]string{
		"model":           s.cfg.TTS.OpenAIModel,
		"input":           strings.TrimSpace(req.Text),
		"voice":           voice,
		"response_format": format,
	})
	if err != nil {
		return nil, "", "", err
	}

	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.TTS.OpenAIURL, bytes.NewReader(body))
	if err != nil {
		return nil, "", "", err
	}
	httpReq.Header.Set("Authorization", "Bearer "+s.cfg.TTS.OpenAIAPIKey)
	httpReq.Header.Set("Content-Type", "application/json")

	audio, contentType, err := s.doAudioRequest(httpReq, "OpenAI TTS")
	if err != nil {
		return nil, "", "", err
	}
	if contentType == "" {
		contentType = contentTypeForFormat(format)
	}
	return audio, contentType, format, nil
}

func (s *server) callElevenLabsTTS(ctx context.Context, req ttsRequest) ([]byte, string, string, error) {
	if s.cfg.TTS.ElevenLabsAPIKey == "" {
		return nil, "", "", errors.New("ELEVENLABS_API_KEY is required for ElevenLabs TTS")
	}

	voiceID := firstNonEmpty(req.Voice, s.cfg.TTS.ElevenLabsVoiceID)
	if voiceID == "" {
		return nil, "", "", errors.New("ELEVENLABS_VOICE_ID is required for ElevenLabs TTS")
	}

	outputFormat := firstNonEmpty(req.Format, s.cfg.TTS.ElevenLabsOutputFormat)
	endpoint, err := url.JoinPath(s.cfg.TTS.ElevenLabsURL, voiceID)
	if err != nil {
		return nil, "", "", err
	}
	endpointURL, err := url.Parse(endpoint)
	if err != nil {
		return nil, "", "", err
	}
	values := endpointURL.Query()
	values.Set("output_format", outputFormat)
	endpointURL.RawQuery = values.Encode()

	body, err := json.Marshal(map[string]any{
		"text":     strings.TrimSpace(req.Text),
		"model_id": s.cfg.TTS.ElevenLabsModel,
	})
	if err != nil {
		return nil, "", "", err
	}

	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost, endpointURL.String(), bytes.NewReader(body))
	if err != nil {
		return nil, "", "", err
	}
	httpReq.Header.Set("Content-Type", "application/json")
	httpReq.Header.Set("xi-api-key", s.cfg.TTS.ElevenLabsAPIKey)

	audio, contentType, err := s.doAudioRequest(httpReq, "ElevenLabs TTS")
	if err != nil {
		return nil, "", "", err
	}
	if contentType == "" {
		contentType = contentTypeForFormat(outputFormat)
	}
	return audio, contentType, outputFormat, nil
}

func (s *server) doAudioRequest(req *http.Request, label string) ([]byte, string, error) {
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, "", err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 16*1024*1024))
	if err != nil {
		return nil, "", err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, "", fmt.Errorf("%s returned HTTP %d: %s", label, resp.StatusCode, string(body))
	}
	return body, resp.Header.Get("Content-Type"), nil
}

func (s *server) analyze(ctx context.Context, mode string, image []byte, priorFrames []frameContext, summary bool) (string, bool, string, error) {
	switch s.cfg.Provider {
	case "fake", "mock", "":
		return fakeMessage(mode, summary), true, "fake", nil
	case "ollama":
		message, err := s.callOllama(ctx, mode, image, priorFrames, summary)
		if err != nil {
			if s.cfg.FallbackOnAIError {
				logWarn("ollama unavailable; returning fallback response", "error", err)
				if summary {
					return fakeMessage(mode, true), true, "ollama-fallback", nil
				}
				return "", false, "ollama-fallback", nil
			}
			return "", false, "ollama", err
		}
		message, shouldRespond := parseModelDecision(message)
		if summary && !shouldRespond {
			return "Session ended. Review the last visible step and choose the next small move.", true, "ollama", nil
		}
		return message, shouldRespond, "ollama", nil
	default:
		return "", false, s.cfg.Provider, fmt.Errorf("unsupported SIDEKICK_AI_PROVIDER %q", s.cfg.Provider)
	}
}

func (s *server) callOllama(_ context.Context, mode string, image []byte, priorFrames []frameContext, summary bool) (string, error) {
	messages := make([]ollamaMessage, 0, len(priorFrames)+1)

	imageCount := 0
	imageBytes := len(image)
	for idx, frame := range priorFrames {
		content := fmt.Sprintf("Prior snapshot %d of %d. Mode=%s. Time=%s. Previous tutor message=%q. Use this only as recent visible context for the current request.",
			idx+1, len(priorFrames), frame.Mode, frame.ObservedAt.Format(time.RFC3339), frame.Message)
		messages = append(messages, ollamaMessage{
			Role:    "user",
			Content: content,
			Images:  []string{base64.StdEncoding.EncodeToString(frame.Image)},
		})
		imageCount++
		imageBytes += len(frame.Image)
	}

	if image != nil {
		messages = append(messages, ollamaMessage{
			Role:    "user",
			Content: snapshotPrompt(mode, summary),
			Images:  []string{base64.StdEncoding.EncodeToString(image)},
		})
		imageCount++
	} else {
		messages = append(messages, ollamaMessage{
			Role:    "user",
			Content: snapshotPrompt(mode, summary),
		})
	}

	payload := ollamaChatRequest{
		Model:    s.cfg.OllamaModel,
		Stream:   false,
		Think:    false,
		Messages: messages,
		Options: ollamaOptions{
			Temperature: 0.2,
			NumPredict:  s.cfg.MaxOutputTokens,
		},
	}

	body, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}

	// Keep the upstream model call diagnostic even if the board HTTP client gives up.
	ollamaCtx, cancel := context.WithTimeout(context.Background(), s.cfg.RequestTimeout)
	defer cancel()

	if s.cfg.Verbose {
		logInfo("ollama request", "model", s.cfg.OllamaModel, "mode", mode, "summary", summary, "images", imageCount, "image_bytes", imageBytes, "payload_bytes", len(body))
	}

	start := time.Now()
	req, err := http.NewRequestWithContext(ollamaCtx, http.MethodPost, s.cfg.OllamaChatURL, bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := s.client.Do(req)
	if err != nil {
		return "", fmt.Errorf("Ollama request failed after %s: %w", time.Since(start).Round(time.Millisecond), err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024))
	if err != nil {
		return "", err
	}

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("Ollama returned HTTP %d: %s", resp.StatusCode, string(respBody))
	}
	if s.cfg.Verbose {
		logInfo("ollama response", "model", s.cfg.OllamaModel, "mode", mode, "latency", time.Since(start).Round(time.Millisecond), "response_bytes", len(respBody))
	}

	var parsed ollamaChatResponse
	if err := json.Unmarshal(respBody, &parsed); err != nil {
		return "", err
	}
	if parsed.Error != "" {
		return "", errors.New(parsed.Error)
	}

	text := cleanModelText(parsed.Message.Content)
	if s.cfg.Verbose {
		logInfo("ollama raw message", "message", text, "thinking_bytes", len(parsed.Message.Thinking), "done_reason", firstNonEmpty(parsed.DoneReason, "unknown"))
	}
	if text == "" {
		if strings.TrimSpace(parsed.Message.Thinking) != "" {
			return "", fmt.Errorf("Ollama response did not include message content; model returned thinking only (done_reason=%s thinking_bytes=%d). Use a non-thinking vision model such as llama3.2-vision:11b or raise SIDEKICK_MAX_OUTPUT_TOKENS.",
				firstNonEmpty(parsed.DoneReason, "unknown"), len(parsed.Message.Thinking))
		}
		return "", fmt.Errorf("Ollama response did not include message content (done_reason=%s)", firstNonEmpty(parsed.DoneReason, "unknown"))
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
	Message    ollamaMessage `json:"message"`
	DoneReason string        `json:"done_reason,omitempty"`
	Error      string        `json:"error,omitempty"`
}

func normalizeMode(mode string) string {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "active", "hint", "summary":
		return strings.ToLower(strings.TrimSpace(mode))
	default:
		return "hint"
	}
}

func snapshotPrompt(mode string, summary bool) string {
	return strings.Join([]string{
		sidekickSnapshotPrompt(),
		sidekickModePrompt(mode, summary),
	}, "\n\n")
}

func sidekickSnapshotPrompt() string {
	return "Each picture creates a fresh analysis branch from this same main role. The current request includes the current snapshot and may include recent prior snapshots from the same session. Compare only the provided snapshots to identify visible progress, repeated unchanged work, or clearly visible mistakes. Prior snapshots provide context, not proof of unseen work. For normal frame analysis, return exactly NO_ACTION when no useful visible intervention is justified."
}

func sidekickModePrompt(mode string, summary bool) string {
	if summary {
		return "The session has ended. Summarize only visible work and visible progress, then give one concrete next step. Use at most 40 words in a single brief paragraph. Do not use bullet points. Never return NO_ACTION."
	}
	switch mode {
	case "active":
		return "Active mode: intervene only when visible evidence shows the student is stuck, has stopped making visible progress, or made a clearly visible mistake. Otherwise return exactly NO_ACTION. If responding, use at most 25 words."
	default:
		return "Hint mode: stay quiet unless a visible, specific hint would help. If no hint is necessary, return exactly NO_ACTION. If responding, use at most 25 words."
	}
}

func fakeMessage(mode string, summary bool) string {
	if summary {
		return "You made visible progress. Review the last step and decide the next small move."
	}
	switch mode {
	case "active":
		return "What is the first thing you can label or simplify here?"
	default:
		return "Try identifying the main equation or diagram first."
	}
}

func parseModelDecision(text string) (string, bool) {
	text = cleanModelText(text)
	decision := strings.Trim(strings.TrimSpace(text), "\"'` .\n\t")
	if strings.EqualFold(decision, "NO_ACTION") {
		return "", false
	}
	return text, strings.TrimSpace(text) != ""
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

func loadCaptureDir() string {
	dir := strings.TrimSpace(os.Getenv("SIDEKICK_CAPTURE_DIR"))
	if dir == "" {
		return defaultCaptureDir
	}
	switch strings.ToLower(dir) {
	case "off", "0", "false", "no":
		return ""
	default:
		return dir
	}
}

func sanitizeCaptureToken(value string) string {
	value = strings.TrimSpace(value)
	if value == "" {
		return "unknown"
	}
	replacer := strings.NewReplacer("/", "_", "\\", "_", ":", "_")
	return replacer.Replace(value)
}

func (s *server) persistCapture(sessionID, mode string, image []byte) {
	if s.cfg.CaptureDir == "" || len(image) == 0 {
		return
	}

	if err := os.MkdirAll(s.cfg.CaptureDir, 0o755); err != nil {
		logError("capture save mkdir failed", "error", err)
		return
	}

	name := fmt.Sprintf("%s_%s_%s.jpg",
		sanitizeCaptureToken(sessionID),
		sanitizeCaptureToken(normalizeMode(mode)),
		time.Now().Format("20060102_150405.000"),
	)
	path := filepath.Join(s.cfg.CaptureDir, name)
	if err := os.WriteFile(path, image, 0o644); err != nil {
		logError("capture save write failed", "error", err)
		return
	}
	logInfo("saved capture", "path", path, "bytes", len(image))
}

func normalizeSessionID(sessionID string) string {
	sessionID = strings.TrimSpace(sessionID)
	if sessionID == "" {
		return defaultSessionID
	}
	return sessionID
}

func (s *server) sessionFrames(sessionID string) []frameContext {
	s.mu.Lock()
	defer s.mu.Unlock()

	state := s.sessions[sessionID]
	if state == nil || len(state.Frames) == 0 {
		return nil
	}

	frames := make([]frameContext, len(state.Frames))
	for idx, frame := range state.Frames {
		frames[idx] = frame
		frames[idx].Image = append([]byte(nil), frame.Image...)
	}
	return frames
}

func (s *server) recordFrame(sessionID string, frame frameContext) {
	s.mu.Lock()
	defer s.mu.Unlock()

	state := s.sessions[sessionID]
	if state == nil {
		state = &sessionState{}
		s.sessions[sessionID] = state
	}

	state.Frames = append(state.Frames, frame)
	maxFrames := s.cfg.ContextFrames
	if maxFrames <= 0 {
		maxFrames = defaultContextFrames
	}
	if len(state.Frames) > maxFrames {
		state.Frames = append([]frameContext(nil), state.Frames[len(state.Frames)-maxFrames:]...)
	}
}

func (s *server) clearSession(sessionID string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.sessions, sessionID)
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

func getenvBool(name string, fallback bool) bool {
	value := strings.ToLower(strings.TrimSpace(os.Getenv(name)))
	if value == "" {
		return fallback
	}
	switch value {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	default:
		return fallback
	}
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value != "" {
			return value
		}
	}
	return ""
}

func contentTypeForFormat(format string) string {
	format = strings.ToLower(format)
	switch {
	case strings.Contains(format, "wav"):
		return "audio/wav"
	case strings.Contains(format, "pcm"):
		return "audio/L16"
	case strings.Contains(format, "opus"):
		return "audio/opus"
	case strings.Contains(format, "aac"):
		return "audio/aac"
	case strings.Contains(format, "flac"):
		return "audio/flac"
	default:
		return "audio/mpeg"
	}
}

const (
	ansiReset   = "\033[0m"
	ansiBold    = "\033[1m"
	ansiDim     = "\033[2m"
	ansiRed     = "\033[31m"
	ansiGreen   = "\033[32m"
	ansiYellow  = "\033[33m"
	ansiBlue    = "\033[34m"
	ansiMagenta = "\033[35m"
	ansiCyan    = "\033[36m"
)

type logLevel int

const (
	levelDebug logLevel = iota
	levelInfo
	levelWarn
	levelError
)

func logCustom(level logLevel, msg string, keysAndValues ...any) {
	ts := time.Now().Format("15:04:05.000")
	fmt.Print(ansiDim + ts + ansiReset + " ")

	var badge string
	switch level {
	case levelDebug:
		badge = ansiBold + ansiMagenta + "• DEBUG" + ansiReset
	case levelInfo:
		badge = ansiBold + ansiBlue + "• INFO " + ansiReset
	case levelWarn:
		badge = ansiBold + ansiYellow + "• WARN " + ansiReset
	case levelError:
		badge = ansiBold + ansiRed + "• ERROR" + ansiReset
	}
	fmt.Print(badge + " " + ansiBold + msg + ansiReset)

	for i := 0; i < len(keysAndValues); i += 2 {
		if i+1 < len(keysAndValues) {
			k := fmt.Sprintf("%v", keysAndValues[i])
			v := fmt.Sprintf("%v", keysAndValues[i+1])
			fmt.Print(" " + ansiDim + k + "=" + ansiReset + ansiCyan + v + ansiReset)
		}
	}
	fmt.Println()
}

func logInfo(msg string, kv ...any) {
	logCustom(levelInfo, msg, kv...)
}

func logDebug(msg string, kv ...any) {
	logCustom(levelDebug, msg, kv...)
}

func logWarn(msg string, kv ...any) {
	logCustom(levelWarn, msg, kv...)
}

func logError(msg string, kv ...any) {
	logCustom(levelError, msg, kv...)
}
