package httpbridge

import (
	"fmt"
	"io"
)

type LogLevel int

const (
	LogLevelDebug LogLevel = iota
	LogLevelInfo
	LogLevelWarn
	LogLevelError
	LogLevelFatal
)

type Logger struct {
	Target io.Writer
	Level  LogLevel
}

func (g *Logger) Debug(m string) {
	g.write(LogLevelDebug, m)
}

func (g *Logger) Debugf(m string, args ...interface{}) {
	g.writef(LogLevelDebug, m, args...)
}

func (g *Logger) Info(m string) {
	g.write(LogLevelInfo, m)
}

func (g *Logger) Infof(m string, args ...interface{}) {
	g.writef(LogLevelInfo, m, args...)
}

func (g *Logger) Warn(m string) {
	g.write(LogLevelWarn, m)
}

func (g *Logger) Warnf(m string, args ...interface{}) {
	g.writef(LogLevelWarn, m, args...)
}

func (g *Logger) Error(m string) {
	g.write(LogLevelError, m)
}

func (g *Logger) Errorf(m string, args ...interface{}) {
	g.writef(LogLevelError, m, args...)
}

func (g *Logger) Fatal(m string) {
	g.write(LogLevelFatal, m)
}

func (g *Logger) Fatalf(m string, args ...interface{}) {
	g.writef(LogLevelFatal, m, args...)
}

func (g *Logger) write(level LogLevel, m string) {
	if level >= g.Level {
		g.Target.Write([]byte(m))
		if len(m) == 0 || m[len(m)-1] != '\n' {
			g.Target.Write([]byte("\n"))
		}
	}
	if level == LogLevelFatal {
		panic(m)
	}
}

func (g *Logger) writef(level LogLevel, m string, args ...interface{}) {
	if level >= g.Level {
		g.write(level, fmt.Sprintf(m, args...))
	}
}
