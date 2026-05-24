package main

import (
	"fmt"
	"time"
)

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
