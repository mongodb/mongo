// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"log"
	"os"
)

const (
	// Always denotes that a log be performed without needing any verbosity
	Always = iota
	// Info denotes that a log be performed with verbosity level 1 (-v)
	Info
	// DebugLow denotes that a log be performed with verbosity level 2 (-vv)
	DebugLow
	// DebugHigh denotes that a log be performed with verbosity level 3 (-vvv)
	DebugHigh
)

var logger *log.Logger
var userInfoLogger *logWrapper
var toolDebugLogger *logWrapper

type logWrapper struct {
	out       *log.Logger
	verbosity int
}

func init() {
	if logger == nil {
		logger = log.New(os.Stderr, "", log.Ldate|log.Ltime)
	}
	if userInfoLogger == nil {
		userInfoLogger = &logWrapper{logger, 0}
	}
	if toolDebugLogger == nil {
		toolDebugLogger = &logWrapper{logger, 0}
	}
}

func (lw *logWrapper) setVerbosity(verbosity int) {
	lw.verbosity = verbosity
}

func (lw *logWrapper) Logvf(minVerb int, format string, a ...interface{}) {
	if minVerb < 0 {
		panic("cannot set a minimum log verbosity that is less than 0")
	}

	if minVerb <= lw.verbosity {
		lw.out.Printf(format, a...)
	}
}

func (lw *logWrapper) Logv(minVerb int, msg string) {
	if minVerb < 0 {
		panic("cannot set a minimum log verbosity that is less than 0")
	}

	if minVerb <= lw.verbosity {
		lw.out.Print(msg)
	}
}
func (lw *logWrapper) isInVerbosity(minVerb int) bool {
	return minVerb <= lw.verbosity
}
