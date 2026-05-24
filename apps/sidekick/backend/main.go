package main

import (
	"flag"
	"log"
	"net/http"
)

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
