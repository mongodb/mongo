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
	"bytes"
	"fmt"
	"log"
	"math"
	"os"
	"os/signal"
	"regexp"
	"strings"
	"syscall"
	"text/template"
)

// SetupConfig is a configuration struct meant to be used with
//   github.com/spacemonkeygo/flagfile/utils.Setup
// but can be used independently.
type SetupConfig struct {
	Output   string `default:"stderr" usage:"log output. can be stdout, stderr, syslog, or a path"`
	Level    string `default:"" usage:"base logger level"`
	Filter   string `default:"" usage:"sets loggers matching this regular expression to the lowest level"`
	Format   string `default:"" usage:"format string to use"`
	Stdlevel string `default:"warn" usage:"logger level for stdlib log integration"`
	Subproc  string `default:"" usage:"process to run for stdout/stderr-captured logging. The command is first processed as a Go template that supports {{.Facility}}, {{.Level}}, and {{.Name}} fields, and then passed to sh. If set, will redirect stdout and stderr to the given process. A good default is 'setsid logger --priority {{.Facility}}.{{.Level}} --tag {{.Name}}'"`
	Buffer   int    `default:"0" usage:"the number of messages to buffer. 0 for no buffer"`
	// Facility defaults to syslog.LOG_USER (which is 8)
	Facility  int  `default:"8" usage:"the syslog facility to use if syslog output is configured"`
	HupRotate bool `default:"false" usage:"if true, sending a HUP signal will reopen log files"`
}

var (
	stdlog  = GetLoggerNamed("stdlog")
	funcmap = template.FuncMap{"ColorizeLevel": ColorizeLevel}
)

// SetFormatMethod adds functions to the template function map, such that
// command-line and Setup provided templates can call methods added to the map
// via this method. The map comes prepopulated with ColorizeLevel, but can be
// overridden. SetFormatMethod should be called (if at all) before one of
// this package's Setup methods.
func SetFormatMethod(name string, fn interface{}) {
	funcmap[name] = fn
}

// MustSetup is the same as Setup, but panics instead of returning an error
func MustSetup(procname string, config SetupConfig) {
	err := Setup(procname, config)
	if err != nil {
		panic(err)
	}
}

type subprocInfo struct {
	Facility string
	Level    string
	Name     string
}

// Setup takes a given procname and sets spacelog up with the given
// configuration. Setup supports:
//  * capturing stdout and stderr to a subprocess
//  * configuring the default level
//  * configuring log filters (enabling only some loggers)
//  * configuring the logging template
//  * configuring the output (a file, syslog, stdout, stderr)
//  * configuring log event buffering
//  * capturing all standard library logging with configurable log level
// It is expected that this method will be called once at process start.
func Setup(procname string, config SetupConfig) error {
	if config.Subproc != "" {
		t, err := template.New("subproc").Parse(config.Subproc)
		if err != nil {
			return err
		}
		var buf bytes.Buffer
		err = t.Execute(&buf, &subprocInfo{
			Facility: fmt.Sprintf("%d", config.Facility),
			Level:    fmt.Sprintf("%d", 2), // syslog.LOG_CRIT
			Name:     procname})
		if err != nil {
			return err
		}
		err = CaptureOutputToProcess("sh", "-c", string(buf.Bytes()))
		if err != nil {
			return err
		}
	}
	if config.Level != "" {
		level_val, err := LevelFromString(config.Level)
		if err != nil {
			return err
		}
		if level_val != DefaultLevel {
			SetLevel(nil, level_val)
		}
	}
	if config.Filter != "" {
		re, err := regexp.Compile(config.Filter)
		if err != nil {
			return err
		}
		SetLevel(re, LogLevel(math.MinInt32))
	}
	var t *template.Template
	if config.Format != "" {
		var err error
		t, err = template.New("user").Funcs(funcmap).Parse(config.Format)
		if err != nil {
			return err
		}
	}
	var textout TextOutput
	switch strings.ToLower(config.Output) {
	case "syslog":
		w, err := NewSyslogOutput(SyslogPriority(config.Facility), procname)
		if err != nil {
			return err
		}
		if t == nil {
			t = SyslogTemplate
		}
		textout = w
	case "stdout":
		if t == nil {
			t = DefaultTemplate
		}
		textout = NewWriterOutput(os.Stdout)
	case "stderr", "":
		if t == nil {
			t = DefaultTemplate
		}
		textout = NewWriterOutput(os.Stderr)
	default:
		if t == nil {
			t = StandardTemplate
		}
		var err error
		textout, err = NewFileWriterOutput(config.Output)
		if err != nil {
			return err
		}
	}
	if config.HupRotate {
		if hh, ok := textout.(HupHandlingTextOutput); ok {
			sigchan := make(chan os.Signal)
			signal.Notify(sigchan, syscall.SIGHUP)
			go func() {
				for _ = range sigchan {
					hh.OnHup()
				}
			}()
		}
	}
	if config.Buffer > 0 {
		textout = NewBufferedOutput(textout, config.Buffer)
	}
	SetHandler(nil, NewTextHandler(t, textout))
	log.SetFlags(log.Lshortfile)
	if config.Stdlevel == "" {
		config.Stdlevel = "warn"
	}
	stdlog_level_val, err := LevelFromString(config.Stdlevel)
	if err != nil {
		return err
	}
	log.SetOutput(stdlog.WriterWithoutCaller(stdlog_level_val))
	return nil
}
