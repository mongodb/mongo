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
	"text/template"
)

// Handler is an interface that knows how to process log events. This is the
// basic interface type for building a logging system. If you want to route
// structured log data somewhere, you would implement this interface.
type Handler interface {
	// Log is called for every message. if calldepth is negative, caller
	// information is missing
	Log(logger_name string, level LogLevel, msg string, calldepth int)

	// These two calls are expected to be no-ops on non-text-output handlers
	SetTextTemplate(t *template.Template)
	SetTextOutput(output TextOutput)
}

// HandlerFunc is a type to make implementation of the Handler interface easier
type HandlerFunc func(logger_name string, level LogLevel, msg string,
	calldepth int)

// Log simply calls f(logger_name, level, msg, calldepth)
func (f HandlerFunc) Log(logger_name string, level LogLevel, msg string,
	calldepth int) {
	f(logger_name, level, msg, calldepth)
}

// SetTextTemplate is a no-op
func (HandlerFunc) SetTextTemplate(t *template.Template) {}

// SetTextOutput is a no-op
func (HandlerFunc) SetTextOutput(output TextOutput) {}

var (
	defaultHandler = NewTextHandler(StdlibTemplate,
		&StdlibOutput{})
)
