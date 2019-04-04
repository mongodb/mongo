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

// ColorizeLevel returns a TermColor byte sequence for the appropriate color
// for the level. If you'd like to configure your own color choices, you can
// make your own template with its own function map to your own colorize
// function.
func ColorizeLevel(level LogLevel) string {
	switch level.Match() {
	case Critical, Error:
		return TermColors{}.Red()
	case Warning:
		return TermColors{}.Magenta()
	case Notice:
		return TermColors{}.Yellow()
	case Info, Debug:
		return TermColors{}.Green()
	}
	return ""
}

var (
	// ColorTemplate uses the default ColorizeLevel method for color choices.
	ColorTemplate = template.Must(template.New("color").Funcs(template.FuncMap{
		"ColorizeLevel": ColorizeLevel}).Parse(
		`{{.Blue}}{{.Date}} {{.Time}}{{.Reset}} ` +
			`{{.Bold}}{{ColorizeLevel .Level}}{{.LevelJustified}}{{.Reset}} ` +
			`{{.Underline}}{{.LoggerName}}{{.Reset}} ` +
			`{{if .Filename}}{{.Filename}}:{{.Line}} {{end}}- ` +
			`{{ColorizeLevel .Level}}{{.Message}}{{.Reset}}`))

	// StandardTemplate is like ColorTemplate with no color.
	StandardTemplate = template.Must(template.New("standard").Parse(
		`{{.Date}} {{.Time}} ` +
			`{{.Level}} {{.LoggerName}} ` +
			`{{if .Filename}}{{.Filename}}:{{.Line}} {{end}}` +
			`- {{.Message}}`))

	// SyslogTemplate is missing the date and time as syslog adds those
	// things.
	SyslogTemplate = template.Must(template.New("syslog").Parse(
		`{{.Level}} {{.LoggerName}} ` +
			`{{if .Filename}}{{.Filename}}:{{.Line}} {{end}}` +
			`- {{.Message}}`))

	// StdlibTemplate is missing the date and time as the stdlib logger often
	// adds those things.
	StdlibTemplate = template.Must(template.New("stdlib").Parse(
		`{{.Level}} {{.LoggerName}} ` +
			`{{if .Filename}}{{.Filename}}:{{.Line}} {{end}}` +
			`- {{.Message}}`))
)
