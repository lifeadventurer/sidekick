package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/textproto"
	"strconv"
	"strings"
)

type transcriptionResponse struct {
	Text string `json:"text"`
}

func queryInt(r *http.Request, name string, fallback int) int {
	value := strings.TrimSpace(r.URL.Query().Get(name))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil || parsed <= 0 {
		return fallback
	}
	return parsed
}

func (s *server) transcribeAudio(ctx context.Context, pcm []byte, sampleRate, channels, bits int) (string, string, error) {
	switch strings.ToLower(strings.TrimSpace(s.cfg.STT.Provider)) {
	case "none", "off", "":
		return "", "none", nil
	case "fake", "mock":
		return "Student asked for help.", "fake", nil
	case "openai":
		text, err := s.callOpenAITranscription(ctx, pcm, sampleRate, channels, bits)
		return text, "openai", err
	case "elevenlabs":
		text, err := s.callElevenLabsTranscription(ctx, pcm, sampleRate, channels, bits)
		return text, "elevenlabs", err
	default:
		return "", s.cfg.STT.Provider, fmt.Errorf("unsupported SIDEKICK_STT_PROVIDER %q", s.cfg.STT.Provider)
	}
}

func (s *server) callElevenLabsTranscription(ctx context.Context, pcm []byte, sampleRate, channels, bits int) (string, error) {
	if s.cfg.STT.ElevenLabsAPIKey == "" {
		return "", errors.New("ELEVENLABS_API_KEY is required for ElevenLabs transcription")
	}
	if sampleRate != 16000 || channels != 1 || bits != 16 {
		return "", fmt.Errorf("unsupported ElevenLabs raw PCM format: sample_rate=%d channels=%d bits=%d", sampleRate, channels, bits)
	}

	var body bytes.Buffer
	writer := multipart.NewWriter(&body)

	header := make(textproto.MIMEHeader)
	header.Set("Content-Disposition", `form-data; name="file"; filename="sidekick.pcm"`)
	header.Set("Content-Type", "application/octet-stream")
	part, err := writer.CreatePart(header)
	if err != nil {
		return "", err
	}
	if _, err := part.Write(pcm); err != nil {
		return "", err
	}
	if err := writer.WriteField("model_id", s.cfg.STT.ElevenLabsModel); err != nil {
		return "", err
	}
	if err := writer.WriteField("file_format", "pcm_s16le_16"); err != nil {
		return "", err
	}
	if err := writer.WriteField("timestamps_granularity", "none"); err != nil {
		return "", err
	}
	if err := writer.WriteField("tag_audio_events", "false"); err != nil {
		return "", err
	}
	if s.cfg.STT.Language != "" {
		if err := writer.WriteField("language_code", s.cfg.STT.Language); err != nil {
			return "", err
		}
	}
	if err := writer.Close(); err != nil {
		return "", err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.STT.ElevenLabsURL, &body)
	if err != nil {
		return "", err
	}
	req.Header.Set("xi-api-key", s.cfg.STT.ElevenLabsAPIKey)
	req.Header.Set("Content-Type", writer.FormDataContentType())

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
		return "", fmt.Errorf("ElevenLabs transcription returned HTTP %d: %s", resp.StatusCode, string(respBody))
	}

	var parsed transcriptionResponse
	if err := json.Unmarshal(respBody, &parsed); err != nil {
		return "", err
	}
	return strings.TrimSpace(parsed.Text), nil
}

func (s *server) callOpenAITranscription(ctx context.Context, pcm []byte, sampleRate, channels, bits int) (string, error) {
	if s.cfg.STT.OpenAIAPIKey == "" {
		return "", errors.New("OPENAI_API_KEY is required for OpenAI transcription")
	}
	if bits != 16 {
		return "", fmt.Errorf("unsupported PCM bit depth %d", bits)
	}

	wav := wrapPCMAsWAV(pcm, sampleRate, channels, bits)
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)

	header := make(textproto.MIMEHeader)
	header.Set("Content-Disposition", `form-data; name="file"; filename="sidekick.wav"`)
	header.Set("Content-Type", "audio/wav")
	part, err := writer.CreatePart(header)
	if err != nil {
		return "", err
	}
	if _, err := part.Write(wav); err != nil {
		return "", err
	}
	if err := writer.WriteField("model", s.cfg.STT.OpenAIModel); err != nil {
		return "", err
	}
	if err := writer.WriteField("response_format", "json"); err != nil {
		return "", err
	}
	if s.cfg.STT.Language != "" {
		if err := writer.WriteField("language", s.cfg.STT.Language); err != nil {
			return "", err
		}
	}
	if err := writer.Close(); err != nil {
		return "", err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.STT.OpenAIURL, &body)
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+s.cfg.STT.OpenAIAPIKey)
	req.Header.Set("Content-Type", writer.FormDataContentType())

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
		return "", fmt.Errorf("OpenAI transcription returned HTTP %d: %s", resp.StatusCode, string(respBody))
	}

	var parsed transcriptionResponse
	if err := json.Unmarshal(respBody, &parsed); err != nil {
		return "", err
	}
	return strings.TrimSpace(parsed.Text), nil
}

func wrapPCMAsWAV(pcm []byte, sampleRate, channels, bits int) []byte {
	if sampleRate <= 0 {
		sampleRate = 16000
	}
	if channels <= 0 {
		channels = 1
	}
	if bits <= 0 {
		bits = 16
	}

	dataLen := uint32(len(pcm))
	byteRate := uint32(sampleRate * channels * bits / 8)
	blockAlign := uint16(channels * bits / 8)

	var out bytes.Buffer
	out.Grow(44 + len(pcm))
	out.WriteString("RIFF")
	_ = binary.Write(&out, binary.LittleEndian, uint32(36)+dataLen)
	out.WriteString("WAVE")
	out.WriteString("fmt ")
	_ = binary.Write(&out, binary.LittleEndian, uint32(16))
	_ = binary.Write(&out, binary.LittleEndian, uint16(1))
	_ = binary.Write(&out, binary.LittleEndian, uint16(channels))
	_ = binary.Write(&out, binary.LittleEndian, uint32(sampleRate))
	_ = binary.Write(&out, binary.LittleEndian, byteRate)
	_ = binary.Write(&out, binary.LittleEndian, blockAlign)
	_ = binary.Write(&out, binary.LittleEndian, uint16(bits))
	out.WriteString("data")
	_ = binary.Write(&out, binary.LittleEndian, dataLen)
	out.Write(pcm)
	return out.Bytes()
}
