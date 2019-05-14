// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	"errors"
)

const (
	ExitSuccess int = iota
	ExitFailure
)

var (
	ErrTerminated = errors.New("received termination signal")
)

func ShortUsage(tool string) string {
	return "try '" + tool + " --help' for more information"
}

// SetupError is the error thrown by "New" functions used to convey what error occurred and the appropriate exit code.
type SetupError struct {
	Err error

	// An optional message to be logged before exiting
	Message string
}

// Error implements the error interface.
func (se SetupError) Error() string {
	return se.Err.Error()
}
