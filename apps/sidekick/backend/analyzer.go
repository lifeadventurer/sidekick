package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

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
	messages := []ollamaMessage{{
		Role:    "system",
		Content: tutorSystemPrompt(mode, summary),
	}}

	imageCount := 0
	imageBytes := len(image)
	for idx, frame := range priorFrames {
		content := fmt.Sprintf("Previous frame %d of %d. Mode=%s. Time=%s. Previous tutor message=%q. Compare this with later frames.",
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
			Content: "Current frame. Compare it with the previous frames, infer progress or lack of motion, and respond with only the tutor message or NO_ACTION.",
			Images:  []string{base64.StdEncoding.EncodeToString(image)},
		})
		imageCount++
	} else {
		messages = append(messages, ollamaMessage{
			Role:    "user",
			Content: "The session has ended. Use the provided frames in order to summarize progress and the best next step.",
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

func normalizeMode(mode string) string {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "active", "hint", "summary":
		return strings.ToLower(strings.TrimSpace(mode))
	default:
		return "hint"
	}
}

func tutorSystemPrompt(mode string, summary bool) string {
	base := "You are SideKick, a visual AI tutor watching ordered snapshots from a student's desk. Compare the current frame with prior frames to infer motion, progress, pauses, and possible wrong direction. Respond with one short message suitable for a tiny device screen. Use at most 25 words. Do not explain your reasoning. Do not mention camera frames or images."
	if summary {
		return base + " The session has ended. Summarize what the student worked on, visible progress, and one concrete next step. Use at most 40 words in a single brief paragraph. Do not use bullet points. Never return NO_ACTION."
	}
	switch mode {
	case "active":
		return base + " Active mode: intervene when the student appears stuck, has stopped changing the work, or is writing something incorrect. Otherwise return exactly NO_ACTION."
	default:
		return base + " Hint mode: stay quiet unless the student is stuck or going the wrong direction. If no hint is necessary, return exactly NO_ACTION."
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
