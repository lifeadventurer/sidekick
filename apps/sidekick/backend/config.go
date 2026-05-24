package main

import (
	"errors"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	defaultPort            = "8787"
	defaultProvider        = "ollama"
	defaultOllamaModel     = "llama3.2-vision:11b"
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
	defaultMaxAudioBytes   = 512 * 1024

	defaultOpenAITTSURL    = "https://api.openai.com/v1/audio/speech"
	defaultOpenAITTSModel  = "gpt-4o-mini-tts"
	defaultOpenAITTSVoice  = "coral"
	defaultOpenAITTSFormat = "wav"

	defaultOpenAITranscriptionURL   = "https://api.openai.com/v1/audio/transcriptions"
	defaultOpenAITranscriptionModel = "gpt-4o-mini-transcribe"

	defaultElevenLabsSTTURL   = "https://api.elevenlabs.io/v1/speech-to-text"
	defaultElevenLabsSTTModel = "scribe_v2"

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
	STT               sttConfig
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

type sttConfig struct {
	Provider         string
	MaxAudioBytes    int64
	OpenAIAPIKey     string
	OpenAIURL        string
	OpenAIModel      string
	ElevenLabsAPIKey string
	ElevenLabsURL    string
	ElevenLabsModel  string
	Language         string
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
		STT: sttConfig{
			Provider:         strings.ToLower(getenv("SIDEKICK_STT_PROVIDER", "elevenlabs")),
			MaxAudioBytes:    getenvInt64("SIDEKICK_MAX_AUDIO_BYTES", defaultMaxAudioBytes),
			OpenAIAPIKey:     os.Getenv("OPENAI_API_KEY"),
			OpenAIURL:        getenv("OPENAI_TRANSCRIPTION_URL", defaultOpenAITranscriptionURL),
			OpenAIModel:      getenv("OPENAI_TRANSCRIPTION_MODEL", defaultOpenAITranscriptionModel),
			ElevenLabsAPIKey: os.Getenv("ELEVENLABS_API_KEY"),
			ElevenLabsURL:    getenv("ELEVENLABS_STT_URL", defaultElevenLabsSTTURL),
			ElevenLabsModel:  getenv("ELEVENLABS_STT_MODEL", defaultElevenLabsSTTModel),
			Language:         getenv("SIDEKICK_STT_LANGUAGE", getenv("OPENAI_TRANSCRIPTION_LANGUAGE", "en")),
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
