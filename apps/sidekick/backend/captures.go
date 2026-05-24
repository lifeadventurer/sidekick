package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

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

func (s *server) recordTranscript(sessionID string, transcript string) {
	s.appendTranscript(sessionID, transcript)
}

func (s *server) appendTranscript(sessionID string, transcript string) string {
	transcript = strings.TrimSpace(transcript)
	if transcript == "" {
		return s.latestTranscript(sessionID)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	state := s.sessions[sessionID]
	if state == nil {
		state = &sessionState{}
		s.sessions[sessionID] = state
	}
	if state.LatestTranscript == "" {
		state.LatestTranscript = transcript
	} else {
		state.LatestTranscript += " " + transcript
	}
	state.TranscriptAt = time.Now()
	return state.LatestTranscript
}

func (s *server) latestTranscript(sessionID string) string {
	s.mu.Lock()
	defer s.mu.Unlock()

	state := s.sessions[sessionID]
	if state == nil {
		return ""
	}
	return state.LatestTranscript
}

func (s *server) clearLatestTranscript(sessionID string, transcript string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	state := s.sessions[sessionID]
	if state == nil {
		return false
	}
	if state.LatestTranscript != transcript {
		return false
	}
	state.LatestTranscript = ""
	state.TranscriptAt = time.Time{}
	return true
}

func (s *server) clearSession(sessionID string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.sessions, sessionID)
}
