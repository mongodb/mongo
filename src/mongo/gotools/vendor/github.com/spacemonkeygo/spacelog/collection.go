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
	"regexp"
	"runtime"
	"strings"
	"sync"
	"text/template"
)

var (
	// If set, these prefixes will be stripped out of automatic logger names.
	IgnoredPrefixes []string

	badChars = regexp.MustCompile("[^a-zA-Z0-9_.-]")
	slashes  = regexp.MustCompile("[/]")
)

func callerName() string {
	pc, _, _, ok := runtime.Caller(2)
	if !ok {
		return "unknown.unknown"
	}
	f := runtime.FuncForPC(pc)
	if f == nil {
		return "unknown.unknown"
	}
	name := f.Name()
	for _, prefix := range IgnoredPrefixes {
		name = strings.TrimPrefix(name, prefix)
	}
	return badChars.ReplaceAllLiteralString(
		slashes.ReplaceAllLiteralString(name, "."), "_")
}

// LoggerCollections contain all of the loggers a program might use. Typically
// a codebase will just use the default logger collection.
type LoggerCollection struct {
	mtx     sync.Mutex
	loggers map[string]*Logger
	level   LogLevel
	handler Handler
}

// NewLoggerCollection creates a new logger collection. It's unlikely you will
// ever practically need this method. Use the DefaultLoggerCollection instead.
func NewLoggerCollection() *LoggerCollection {
	return &LoggerCollection{
		loggers: make(map[string]*Logger),
		level:   DefaultLevel,
		handler: defaultHandler}
}

// GetLogger returns a new Logger with a name automatically generated using
// the callstack. If you want to avoid automatic name generation check out
// GetLoggerNamed
func (c *LoggerCollection) GetLogger() *Logger {
	return GetLoggerNamed(callerName())
}

func (c *LoggerCollection) getLogger(name string, level LogLevel,
	handler Handler) *Logger {
	c.mtx.Lock()
	defer c.mtx.Unlock()

	logger, exists := c.loggers[name]
	if !exists {
		logger = &Logger{level: level,
			collection: c,
			name:       name,
			handler:    handler}
		c.loggers[name] = logger
	}
	return logger
}

// GetLoggerNamed returns a new Logger with the provided name. GetLogger is
// more frequently used.
func (c *LoggerCollection) GetLoggerNamed(name string) *Logger {
	c.mtx.Lock()
	defer c.mtx.Unlock()

	logger, exists := c.loggers[name]
	if !exists {
		logger = &Logger{level: c.level,
			collection: c,
			name:       name,
			handler:    c.handler}
		c.loggers[name] = logger
	}
	return logger
}

// SetLevel will set the current log level for all loggers with names that
// match a provided regular expression. If the regular expression is nil, then
// all loggers match.
func (c *LoggerCollection) SetLevel(re *regexp.Regexp, level LogLevel) {
	c.mtx.Lock()
	defer c.mtx.Unlock()

	if re == nil {
		c.level = level
	}
	for name, logger := range c.loggers {
		if re == nil || re.MatchString(name) {
			logger.setLevel(level)
		}
	}
}

// SetHandler will set the current log handler for all loggers with names that
// match a provided regular expression. If the regular expression is nil, then
// all loggers match.
func (c *LoggerCollection) SetHandler(re *regexp.Regexp, handler Handler) {
	c.mtx.Lock()
	defer c.mtx.Unlock()

	if re == nil {
		c.handler = handler
	}
	for name, logger := range c.loggers {
		if re == nil || re.MatchString(name) {
			logger.setHandler(handler)
		}
	}
}

// SetTextTemplate will set the current text template for all loggers with
// names that match a provided regular expression. If the regular expression
// is nil, then all loggers match. Note that not every handler is guaranteed
// to support text templates and a text template will only apply to
// text-oriented and unstructured handlers.
func (c *LoggerCollection) SetTextTemplate(re *regexp.Regexp,
	t *template.Template) {
	c.mtx.Lock()
	defer c.mtx.Unlock()

	if re == nil {
		c.handler.SetTextTemplate(t)
	}
	for name, logger := range c.loggers {
		if re == nil || re.MatchString(name) {
			logger.getHandler().SetTextTemplate(t)
		}
	}
}

// SetTextOutput will set the current output interface for all loggers with
// names that match a provided regular expression. If the regular expression
// is nil, then all loggers match. Note that not every handler is guaranteed
// to support text output and a text output interface will only apply to
// text-oriented and unstructured handlers.
func (c *LoggerCollection) SetTextOutput(re *regexp.Regexp,
	output TextOutput) {
	c.mtx.Lock()
	defer c.mtx.Unlock()

	if re == nil {
		c.handler.SetTextOutput(output)
	}
	for name, logger := range c.loggers {
		if re == nil || re.MatchString(name) {
			logger.getHandler().SetTextOutput(output)
		}
	}
}

var (
	// It's unlikely you'll need to use this directly
	DefaultLoggerCollection = NewLoggerCollection()
)

// GetLogger returns an automatically-named logger on the default logger
// collection.
func GetLogger() *Logger {
	return DefaultLoggerCollection.GetLoggerNamed(callerName())
}

// GetLoggerNamed returns a new Logger with the provided name on the default
// logger collection. GetLogger is more frequently used.
func GetLoggerNamed(name string) *Logger {
	return DefaultLoggerCollection.GetLoggerNamed(name)
}

// SetLevel will set the current log level for all loggers on the default
// collection with names that match a provided regular expression. If the
// regular expression is nil, then all loggers match.
func SetLevel(re *regexp.Regexp, level LogLevel) {
	DefaultLoggerCollection.SetLevel(re, level)
}

// SetHandler will set the current log handler for all loggers on the default
// collection with names that match a provided regular expression. If the
// regular expression is nil, then all loggers match.
func SetHandler(re *regexp.Regexp, handler Handler) {
	DefaultLoggerCollection.SetHandler(re, handler)
}

// SetTextTemplate will set the current text template for all loggers on the
// default collection with names that match a provided regular expression. If
// the regular expression is nil, then all loggers match. Note that not every
// handler is guaranteed to support text templates and a text template will
// only apply to text-oriented and unstructured handlers.
func SetTextTemplate(re *regexp.Regexp, t *template.Template) {
	DefaultLoggerCollection.SetTextTemplate(re, t)
}

// SetTextOutput will set the current output interface for all loggers on the
// default collection with names that match a provided regular expression. If
// the regular expression is nil, then all loggers match. Note that not every
// handler is guaranteed to support text output and a text output interface
// will only apply to text-oriented and unstructured handlers.
func SetTextOutput(re *regexp.Regexp, output TextOutput) {
	DefaultLoggerCollection.SetTextOutput(re, output)
}
