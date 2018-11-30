// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package stat_consumer

import (
	"sync/atomic"

	"github.com/mongodb/mongo-tools/mongostat/stat_consumer/line"
)

// A LineFormatter formats StatLines for printing.
type LineFormatter interface {
	// FormatLines returns the string representation of the StatLines that are passed in.
	FormatLines(lines []*line.StatLine, headerKeys []string, keyNames map[string]string) string

	// IsFinished returns true iff the formatter cannot print any more data
	IsFinished() bool
	// Finish() is called whem mongostat is shutting down so that the fomatter can clean up
	Finish()
}

type limitableFormatter struct {
	// atomic operations are performed on rowCount, so these two variables
	// should stay at the beginning for the sake of variable alignment
	maxRows, rowCount int64
}

func (lf *limitableFormatter) increment() {
	atomic.AddInt64(&lf.rowCount, 1)
}

func (lf *limitableFormatter) IsFinished() bool {
	return lf.maxRows > 0 && atomic.LoadInt64(&lf.rowCount) >= lf.maxRows
}

type FormatterConstructor func(maxRows int64, includeHeader bool) LineFormatter

var FormatterConstructors = map[string]FormatterConstructor{}
