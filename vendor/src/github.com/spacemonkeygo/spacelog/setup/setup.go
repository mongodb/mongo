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

/*
Package setup provides simple helpers for configuring spacelog from flags.

This package adds the following flags:
  --log.output - can either be stdout, stderr, syslog, or a file path
  --log.level - the base logger level
  --log.filter - loggers that match this regular expression get set to the
      lowest level
  --log.format - a go text template for log lines
  --log.stdlevel - the logger level to assume the standard library logger is
      using
  --log.subproc - a process to run for stdout/stderr capturing
  --log.buffer - the number of message to buffer
*/
package setup

import (
	"github.com/spacemonkeygo/flagfile/utils"
	"github.com/spacemonkeygo/spacelog"
)

var (
	config spacelog.SetupConfig
)

func init() {
	utils.Setup("log", &config)
}

// SetFormatMethod in this subpackage is deprecated and will be removed soon.
// Please see spacelog.SetFormatMethod instead
func SetFormatMethod(name string, fn interface{}) {
	spacelog.SetFormatMethod(name, fn)
}

// MustSetup calls spacelog.MustSetup with a flag-configured config struct
// It's pretty useless to call this method without parsing flags first, via
// flagfile.Load()
func MustSetup(procname string) {
	spacelog.MustSetup(procname, config)
}

// Setup calls spacelog.Setup with a flag-configured config struct
// It's pretty useless to call this method without parsing flags first, via
// flagfile.Load()
func Setup(procname string) error {
	return spacelog.Setup(procname, config)
}

// MustSetupWithFacility is deprecated and will be removed soon. Please
// configure facility through the facility flag option.
func MustSetupWithFacility(procname string, facility spacelog.SyslogPriority) {
	err := SetupWithFacility(procname, facility)
	if err != nil {
		panic(err)
	}
}

// SetupWithFacility is deprecated and will be removed soon. Please
// configure facility through the facility flag option.
func SetupWithFacility(procname string,
	facility spacelog.SyslogPriority) error {
	config_copy := config
	config_copy.Facility = int(facility)
	return spacelog.Setup(procname, config_copy)
}
