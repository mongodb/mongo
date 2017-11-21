// Copyright (C) MongoDB, Inc. 2015-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on gopkg.io/mgo.v2 by Gustavo Niemeyer.
// See THIRD-PARTY-NOTICES for original license terms.

package mgo

import (
	"fmt"
	"sync"
)

// ---------------------------------------------------------------------------
// Logging integration.

// Avoid importing the log type information unnecessarily.  There's a small cost
// associated with using an interface rather than the type.  Depending on how
// often the logger is plugged in, it would be worth using the type instead.
type log_Logger interface {
	Output(calldepth int, s string) error
}

var (
	globalLogger log_Logger
	globalDebug  bool
	globalMutex  sync.Mutex
)

// RACE WARNING: There are known data races when logging, which are manually
// silenced when the race detector is in use. These data races won't be
// observed in typical use, because logging is supposed to be set up once when
// the application starts. Having raceDetector as a constant, the compiler
// should elide the locks altogether in actual use.

// Specify the *log.Logger object where log messages should be sent to.
func SetLogger(logger log_Logger) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	globalLogger = logger
}

// Enable the delivery of debug messages to the logger.  Only meaningful
// if a logger is also set.
func SetDebug(debug bool) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	globalDebug = debug
}

func log(v ...interface{}) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	if globalLogger != nil {
		globalLogger.Output(2, fmt.Sprint(v...))
	}
}

func logln(v ...interface{}) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	if globalLogger != nil {
		globalLogger.Output(2, fmt.Sprintln(v...))
	}
}

func logf(format string, v ...interface{}) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	if globalLogger != nil {
		globalLogger.Output(2, fmt.Sprintf(format, v...))
	}
}

func debug(v ...interface{}) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	if globalDebug && globalLogger != nil {
		globalLogger.Output(2, fmt.Sprint(v...))
	}
}

func debugln(v ...interface{}) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	if globalDebug && globalLogger != nil {
		globalLogger.Output(2, fmt.Sprintln(v...))
	}
}

func debugf(format string, v ...interface{}) {
	if raceDetector {
		globalMutex.Lock()
		defer globalMutex.Unlock()
	}
	if globalDebug && globalLogger != nil {
		globalLogger.Output(2, fmt.Sprintf(format, v...))
	}
}
