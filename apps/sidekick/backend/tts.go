package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type ttsRequest struct {
	Text     string `json:"text"`
	Provider string `json:"provider,omitempty"`
	Voice    string `json:"voice,omitempty"`
	Format   string `json:"format,omitempty"`
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
