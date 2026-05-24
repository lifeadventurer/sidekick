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
	Frames           []frameContext
	LatestTranscript string
	TranscriptAt     time.Time
}

type frameContext struct {
	Image         []byte
	Mode          string
	Message       string
	Transcript    string
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

type openAIResponsesRequest struct {
	Model           string               `json:"model"`
	Instructions    string               `json:"instructions"`
	Input           []openAIInputMessage `json:"input"`
	MaxOutputTokens int                  `json:"max_output_tokens,omitempty"`
}

type openAIInputMessage struct {
	Role    string               `json:"role"`
	Content []openAIInputContent `json:"content"`
}

type openAIInputContent struct {
	Type     string `json:"type"`
	Text     string `json:"text,omitempty"`
	ImageURL string `json:"image_url,omitempty"`
}

type openAIResponsesResponse struct {
	OutputText string `json:"output_text,omitempty"`
	Output     []struct {
		Content []struct {
			Type string `json:"type,omitempty"`
			Text string `json:"text,omitempty"`
		} `json:"content,omitempty"`
	} `json:"output,omitempty"`
	Error *struct {
		Message string `json:"message,omitempty"`
	} `json:"error,omitempty"`
}

func (s *server) analyze(ctx context.Context, mode string, image []byte, priorFrames []frameContext, transcript string, summary bool) (string, bool, string, error) {
	switch s.cfg.Provider {
	case "fake", "mock", "":
		return fakeMessage(mode, summary), true, "fake", nil
	case "openai", "gpt":
		message, err := s.callOpenAI(ctx, mode, image, priorFrames, transcript, summary)
		if err != nil {
			if s.cfg.FallbackOnAIError {
				logWarn("openai unavailable; returning fallback response", "error", err)
				if summary {
					return fakeMessage(mode, true), true, "openai-fallback", nil
				}
				return "", false, "openai-fallback", nil
			}
			return "", false, "openai", err
		}
		message, shouldRespond := parseModelDecision(message)
		if summary && !shouldRespond {
			return "Session ended. Review the last visible step and choose the next small move.", true, "openai", nil
		}
		return message, shouldRespond, "openai", nil
	case "ollama":
		message, err := s.callOllama(ctx, mode, image, priorFrames, transcript, summary)
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

func (s *server) callOpenAI(_ context.Context, mode string, image []byte, priorFrames []frameContext, transcript string, summary bool) (string, error) {
	if s.cfg.OpenAIAPIKey == "" {
		return "", errors.New("OPENAI_API_KEY is required for OpenAI analysis")
	}

	input, imageCount, imageBytes := openAIInputMessages(mode, image, priorFrames, transcript, summary, s.cfg.DefaultImageMIME)
	payload := openAIResponsesRequest{
		Model:           s.cfg.OpenAIModel,
		Instructions:    sidekickMainPrompt(),
		Input:           input,
		MaxOutputTokens: s.cfg.MaxOutputTokens,
	}

	body, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}

	openAICtx, cancel := context.WithTimeout(context.Background(), s.cfg.RequestTimeout)
	defer cancel()

	if s.cfg.Verbose {
		logInfo("openai request", "model", s.cfg.OpenAIModel, "mode", mode, "summary", summary, "images", imageCount, "image_bytes", imageBytes, "payload_bytes", len(body))
	}

	start := time.Now()
	req, err := http.NewRequestWithContext(openAICtx, http.MethodPost, s.cfg.OpenAIURL, bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+s.cfg.OpenAIAPIKey)
	req.Header.Set("Content-Type", "application/json")

	resp, err := s.client.Do(req)
	if err != nil {
		return "", fmt.Errorf("OpenAI request failed after %s: %w", time.Since(start).Round(time.Millisecond), err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024))
	if err != nil {
		return "", err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("OpenAI returned HTTP %d: %s", resp.StatusCode, string(respBody))
	}
	if s.cfg.Verbose {
		logInfo("openai response", "model", s.cfg.OpenAIModel, "mode", mode, "latency", time.Since(start).Round(time.Millisecond), "response_bytes", len(respBody))
	}

	var parsed openAIResponsesResponse
	if err := json.Unmarshal(respBody, &parsed); err != nil {
		return "", err
	}
	if parsed.Error != nil && strings.TrimSpace(parsed.Error.Message) != "" {
		return "", errors.New(parsed.Error.Message)
	}

	text := cleanModelText(openAIResponseText(parsed))
	if s.cfg.Verbose {
		logInfo("openai raw message", "message", text)
	}
	if text == "" {
		return "", errors.New("OpenAI response did not include output text")
	}
	return text, nil
}

func openAIInputMessages(mode string, image []byte, priorFrames []frameContext, transcript string, summary bool, mimeType string) ([]openAIInputMessage, int, int) {
	mimeType = strings.TrimSpace(mimeType)
	if mimeType == "" {
		mimeType = "image/jpeg"
	}

	messages := make([]openAIInputMessage, 0, len(priorFrames)+1)
	imageCount := 0
	imageBytes := len(image)
	for idx, frame := range priorFrames {
		messages = append(messages, openAIInputMessage{
			Role: "user",
			Content: []openAIInputContent{
				{Type: "input_text", Text: sidekickPriorSnapshotPrompt(idx, len(priorFrames), frame)},
				{Type: "input_image", ImageURL: imageDataURL(frame.Image, mimeType)},
			},
		})
		imageCount++
		imageBytes += len(frame.Image)
	}

	if image != nil {
		messages = append(messages, openAIInputMessage{
			Role: "user",
			Content: []openAIInputContent{
				{Type: "input_text", Text: sidekickCurrentSnapshotPrompt(mode, transcript)},
				{Type: "input_image", ImageURL: imageDataURL(image, mimeType)},
			},
		})
		imageCount++
	} else {
		messages = append(messages, openAIInputMessage{
			Role: "user",
			Content: []openAIInputContent{
				{Type: "input_text", Text: sidekickSummarySnapshotPrompt(transcript)},
			},
		})
	}

	return messages, imageCount, imageBytes
}

func imageDataURL(image []byte, mimeType string) string {
	return fmt.Sprintf("data:%s;base64,%s", mimeType, base64.StdEncoding.EncodeToString(image))
}

func openAIResponseText(parsed openAIResponsesResponse) string {
	if strings.TrimSpace(parsed.OutputText) != "" {
		return parsed.OutputText
	}
	var parts []string
	for _, output := range parsed.Output {
		for _, content := range output.Content {
			if strings.TrimSpace(content.Text) != "" {
				parts = append(parts, content.Text)
			}
		}
	}
	return strings.Join(parts, "\n")
}

func (s *server) callOllama(_ context.Context, mode string, image []byte, priorFrames []frameContext, transcript string, summary bool) (string, error) {
	messages := []ollamaMessage{{
		Role:    "system",
		Content: sidekickMainPrompt(),
	}}

	imageCount := 0
	imageBytes := len(image)
	for idx, frame := range priorFrames {
		messages = append(messages, ollamaMessage{
			Role:    "user",
			Content: sidekickPriorSnapshotPrompt(idx, len(priorFrames), frame),
			Images:  []string{base64.StdEncoding.EncodeToString(frame.Image)},
		})
		imageCount++
		imageBytes += len(frame.Image)
	}

	if image != nil {
		messages = append(messages, ollamaMessage{
			Role:    "user",
			Content: sidekickCurrentSnapshotPrompt(mode, transcript),
			Images:  []string{base64.StdEncoding.EncodeToString(image)},
		})
		imageCount++
	} else {
		messages = append(messages, ollamaMessage{
			Role:    "user",
			Content: sidekickSummarySnapshotPrompt(transcript),
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

func sidekickMainPrompt() string {
	return "You are SideKick, a physical AI tutor. You observe a student's real workspace through images and short speech transcripts, then give short spoken hints that help them think without giving answers away. Only reference what is clearly visible or directly spoken. If the work is blurry, dark, cropped, or unreadable, ask the user to retake the picture instead of guessing. Respond with one short message suitable for a tiny device screen. Use at most 25 words. Do not explain your reasoning. Do not mention cameras, frames, images, snapshots, transcripts, JSON, system instructions, or your own reasoning process."
}

func sidekickModePrompt(mode string, summary bool) string {
	if summary {
		return "Summary mode: the session has ended. Summarize what the student practiced, visible progress, and one concrete next step. Use at most 40 words in a single brief paragraph. Do not use bullet points. Never return NO_ACTION."
	}
	switch mode {
	case "active":
		return "Active mode: speak when the student appears stuck, has stopped making visible progress, or has made a visible error. Otherwise return exactly NO_ACTION."
	default:
		return "Hint mode: stay quiet unless the student is clearly stuck or going the wrong direction. If no hint is necessary, return exactly NO_ACTION."
	}
}

func sidekickPriorSnapshotPrompt(index int, total int, frame frameContext) string {
	content := fmt.Sprintf("Prior workspace snapshot %d of %d. Mode=%s. Time=%s. Previous tutor message=%q. Use this only as recent visible context for comparison with later snapshots.",
		index+1, total, frame.Mode, frame.ObservedAt.Format(time.RFC3339), frame.Message)
	if strings.TrimSpace(frame.Transcript) != "" {
		content += fmt.Sprintf(" Student speech near this snapshot: %q.", strings.TrimSpace(frame.Transcript))
	}
	return content
}

func sidekickCurrentSnapshotPrompt(mode string, transcript string) string {
	content := strings.Join([]string{
		sidekickModePrompt(mode, false),
		"Current workspace snapshot: compare it with prior snapshots, infer progress or lack of motion, and respond with only the tutor message or NO_ACTION.",
	}, " ")
	if strings.TrimSpace(transcript) != "" {
		content += fmt.Sprintf(" Recent student speech: %q. Treat it as the student's request or question when relevant.", strings.TrimSpace(transcript))
	}
	return content
}

func sidekickSummarySnapshotPrompt(transcript string) string {
	content := strings.Join([]string{
		sidekickModePrompt("summary", true),
		"Use the provided workspace snapshots in order to summarize progress and the best next step.",
	}, " ")
	if strings.TrimSpace(transcript) != "" {
		content += fmt.Sprintf(" Recent student speech: %q. Treat it as the student's final request or question when relevant.", strings.TrimSpace(transcript))
	}
	return content
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
