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
	"strconv"
	"strings"
)

type LogLevel int32

const (
	Debug    LogLevel = 10
	Info     LogLevel = 20
	Notice   LogLevel = 30
	Warning  LogLevel = 40
	Error    LogLevel = 50
	Critical LogLevel = 60
	// syslog has Alert
	// syslog has Emerg

	DefaultLevel = Notice
)

// String returns the log level name in short form
func (l LogLevel) String() string {
	switch l.Match() {
	case Critical:
		return "CRIT"
	case Error:
		return "ERR"
	case Warning:
		return "WARN"
	case Notice:
		return "NOTE"
	case Info:
		return "INFO"
	case Debug:
		return "DEBUG"
	default:
		return "UNSET"
	}
}

// String returns the log level name in long human readable form
func (l LogLevel) Name() string {
	switch l.Match() {
	case Critical:
		return "critical"
	case Error:
		return "error"
	case Warning:
		return "warning"
	case Notice:
		return "notice"
	case Info:
		return "info"
	case Debug:
		return "debug"
	default:
		return "unset"
	}
}

// Match returns the greatest named log level that is less than or equal to
// the receiver log level. For example, if the log level is 43, Match() will
// return 40 (Warning)
func (l LogLevel) Match() LogLevel {
	if l >= Critical {
		return Critical
	}
	if l >= Error {
		return Error
	}
	if l >= Warning {
		return Warning
	}
	if l >= Notice {
		return Notice
	}
	if l >= Info {
		return Info
	}
	if l >= Debug {
		return Debug
	}
	return 0
}

// LevelFromString will convert a named log level to its corresponding value
// type, or error if both the name was unknown and an integer value was unable
// to be parsed.
func LevelFromString(str string) (LogLevel, error) {
	switch strings.ToLower(str) {
	case "crit", "critical":
		return Critical, nil
	case "err", "error":
		return Error, nil
	case "warn", "warning":
		return Warning, nil
	case "note", "notice":
		return Notice, nil
	case "info":
		return Info, nil
	case "debug":
		return Debug, nil
	}
	val, err := strconv.ParseInt(str, 10, 32)
	if err == nil {
		return LogLevel(val), nil
	}
	return 0, fmt.Errorf("Invalid log level: %s", str)
}
