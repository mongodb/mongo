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

// +build !windows

package spacelog

import (
	"bytes"
	"log/syslog"
)

type SyslogPriority syslog.Priority

// SyslogOutput is a syslog client that matches the TextOutput interface
type SyslogOutput struct {
	w *syslog.Writer
}

// NewSyslogOutput returns a TextOutput object that writes to syslog using
// the given facility and tag. The log level will be determined by the log
// event.
func NewSyslogOutput(facility SyslogPriority, tag string) (
	TextOutput, error) {
	w, err := syslog.New(syslog.Priority(facility), tag)
	if err != nil {
		return nil, err
	}
	return &SyslogOutput{w: w}, nil
}

func (o *SyslogOutput) Output(level LogLevel, message []byte) {
	level = level.Match()
	for _, msg := range bytes.Split(message, []byte{'\n'}) {
		switch level {
		case Critical:
			o.w.Crit(string(msg))
		case Error:
			o.w.Err(string(msg))
		case Warning:
			o.w.Warning(string(msg))
		case Notice:
			o.w.Notice(string(msg))
		case Info:
			o.w.Info(string(msg))
		case Debug:
			fallthrough
		default:
			o.w.Debug(string(msg))
		}
	}
}
