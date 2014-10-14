// Copyright (C) 2014 Space Monkey, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package spacelog

import (
	"fmt"
	"io"
)

// Debug logs a collection of values if the logger's level is debug or even
// more permissive.
func (l *Logger) Debug(v ...interface{}) {
	if l.getLevel() <= Debug {
		l.getHandler().Log(l.name, Debug, fmt.Sprint(v...), 1)
	}
}

// Debugf logs a format string with values if the logger's level is debug or
// even more permissive.
func (l *Logger) Debugf(format string, v ...interface{}) {
	if l.getLevel() <= Debug {
		l.getHandler().Log(l.name, Debug, fmt.Sprintf(format, v...), 1)
	}
}

// Debuge logs an error value if the error is not nil and the logger's level
// is debug or even more permissive.
func (l *Logger) Debuge(err error) {
	if l.getLevel() <= Debug && err != nil {
		l.getHandler().Log(l.name, Debug, err.Error(), 1)
	}
}

// DebugEnabled returns true if the logger's level is debug or even more
// permissive.
func (l *Logger) DebugEnabled() bool {
	return l.getLevel() <= Debug
}

// Info logs a collection of values if the logger's level is info or even
// more permissive.
func (l *Logger) Info(v ...interface{}) {
	if l.getLevel() <= Info {
		l.getHandler().Log(l.name, Info, fmt.Sprint(v...), 1)
	}
}

// Infof logs a format string with values if the logger's level is info or
// even more permissive.
func (l *Logger) Infof(format string, v ...interface{}) {
	if l.getLevel() <= Info {
		l.getHandler().Log(l.name, Info, fmt.Sprintf(format, v...), 1)
	}
}

// Infoe logs an error value if the error is not nil and the logger's level
// is info or even more permissive.
func (l *Logger) Infoe(err error) {
	if l.getLevel() <= Info && err != nil {
		l.getHandler().Log(l.name, Info, err.Error(), 1)
	}
}

// InfoEnabled returns true if the logger's level is info or even more
// permissive.
func (l *Logger) InfoEnabled() bool {
	return l.getLevel() <= Info
}

// Notice logs a collection of values if the logger's level is notice or even
// more permissive.
func (l *Logger) Notice(v ...interface{}) {
	if l.getLevel() <= Notice {
		l.getHandler().Log(l.name, Notice, fmt.Sprint(v...), 1)
	}
}

// Noticef logs a format string with values if the logger's level is notice or
// even more permissive.
func (l *Logger) Noticef(format string, v ...interface{}) {
	if l.getLevel() <= Notice {
		l.getHandler().Log(l.name, Notice, fmt.Sprintf(format, v...), 1)
	}
}

// Noticee logs an error value if the error is not nil and the logger's level
// is notice or even more permissive.
func (l *Logger) Noticee(err error) {
	if l.getLevel() <= Notice && err != nil {
		l.getHandler().Log(l.name, Notice, err.Error(), 1)
	}
}

// NoticeEnabled returns true if the logger's level is notice or even more
// permissive.
func (l *Logger) NoticeEnabled() bool {
	return l.getLevel() <= Notice
}

// Warn logs a collection of values if the logger's level is warning or even
// more permissive.
func (l *Logger) Warn(v ...interface{}) {
	if l.getLevel() <= Warning {
		l.getHandler().Log(l.name, Warning, fmt.Sprint(v...), 1)
	}
}

// Warnf logs a format string with values if the logger's level is warning or
// even more permissive.
func (l *Logger) Warnf(format string, v ...interface{}) {
	if l.getLevel() <= Warning {
		l.getHandler().Log(l.name, Warning, fmt.Sprintf(format, v...), 1)
	}
}

// Warne logs an error value if the error is not nil and the logger's level
// is warning or even more permissive.
func (l *Logger) Warne(err error) {
	if l.getLevel() <= Warning && err != nil {
		l.getHandler().Log(l.name, Warning, err.Error(), 1)
	}
}

// WarnEnabled returns true if the logger's level is warning or even more
// permissive.
func (l *Logger) WarnEnabled() bool {
	return l.getLevel() <= Warning
}

// Error logs a collection of values if the logger's level is error or even
// more permissive.
func (l *Logger) Error(v ...interface{}) {
	if l.getLevel() <= Error {
		l.getHandler().Log(l.name, Error, fmt.Sprint(v...), 1)
	}
}

// Errorf logs a format string with values if the logger's level is error or
// even more permissive.
func (l *Logger) Errorf(format string, v ...interface{}) {
	if l.getLevel() <= Error {
		l.getHandler().Log(l.name, Error, fmt.Sprintf(format, v...), 1)
	}
}

// Errore logs an error value if the error is not nil and the logger's level
// is error or even more permissive.
func (l *Logger) Errore(err error) {
	if l.getLevel() <= Error && err != nil {
		l.getHandler().Log(l.name, Error, err.Error(), 1)
	}
}

// ErrorEnabled returns true if the logger's level is error or even more
// permissive.
func (l *Logger) ErrorEnabled() bool {
	return l.getLevel() <= Error
}

// Crit logs a collection of values if the logger's level is critical or even
// more permissive.
func (l *Logger) Crit(v ...interface{}) {
	if l.getLevel() <= Critical {
		l.getHandler().Log(l.name, Critical, fmt.Sprint(v...), 1)
	}
}

// Critf logs a format string with values if the logger's level is critical or
// even more permissive.
func (l *Logger) Critf(format string, v ...interface{}) {
	if l.getLevel() <= Critical {
		l.getHandler().Log(l.name, Critical, fmt.Sprintf(format, v...), 1)
	}
}

// Crite logs an error value if the error is not nil and the logger's level
// is critical or even more permissive.
func (l *Logger) Crite(err error) {
	if l.getLevel() <= Critical && err != nil {
		l.getHandler().Log(l.name, Critical, err.Error(), 1)
	}
}

// CritEnabled returns true if the logger's level is critical or even more
// permissive.
func (l *Logger) CritEnabled() bool {
	return l.getLevel() <= Critical
}

// Log logs a collection of values if the logger's level is the provided level
// or even more permissive.
func (l *Logger) Log(level LogLevel, v ...interface{}) {
	if l.getLevel() <= level {
		l.getHandler().Log(l.name, level, fmt.Sprint(v...), 1)
	}
}

// Logf logs a format string with values if the logger's level is the provided
// level or even more permissive.
func (l *Logger) Logf(level LogLevel, format string, v ...interface{}) {
	if l.getLevel() <= level {
		l.getHandler().Log(l.name, level, fmt.Sprintf(format, v...), 1)
	}
}

// Loge logs an error value if the error is not nil and the logger's level
// is the provided level or even more permissive.
func (l *Logger) Loge(level LogLevel, err error) {
	if l.getLevel() <= level && err != nil {
		l.getHandler().Log(l.name, level, err.Error(), 1)
	}
}

// LevelEnabled returns true if the logger's level is the provided level or
// even more permissive.
func (l *Logger) LevelEnabled(level LogLevel) bool {
	return l.getLevel() <= level
}

type writer struct {
	l     *Logger
	level LogLevel
}

func (w *writer) Write(data []byte) (int, error) {
	if w.l.getLevel() <= w.level {
		w.l.getHandler().Log(w.l.name, w.level, string(data), 1)
	}
	return len(data), nil
}

// Writer returns an io.Writer that writes messages at the given log level.
func (l *Logger) Writer(level LogLevel) io.Writer {
	return &writer{l: l, level: level}
}

type writerNoCaller struct {
	l     *Logger
	level LogLevel
}

func (w *writerNoCaller) Write(data []byte) (int, error) {
	if w.l.getLevel() <= w.level {
		w.l.getHandler().Log(w.l.name, w.level, string(data), -1)
	}
	return len(data), nil
}

// WriterWithoutCaller returns an io.Writer that writes messages at the given
// log level, but does not attempt to collect the Write caller, and provides
// no caller information to the log event.
func (l *Logger) WriterWithoutCaller(level LogLevel) io.Writer {
	return &writerNoCaller{l: l, level: level}
}
