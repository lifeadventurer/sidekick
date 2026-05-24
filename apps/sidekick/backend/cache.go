package main

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type ttsCacheEntry struct {
	Audio       []byte
	ContentType string
	Format      string
	CreatedAt   time.Time
}

type ttsMetadata struct {
	Key         string    `json:"key"`
	Text        string    `json:"text"`
	Format      string    `json:"format"`
	ContentType string    `json:"content_type"`
	CreatedAt   time.Time `json:"created_at"`
	AudioFile   string    `json:"audio_file"`
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
