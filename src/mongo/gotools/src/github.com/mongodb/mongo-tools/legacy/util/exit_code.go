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
	ExitError      int = 1
	ExitClean      int = 0
	ExitBadOptions int = 3
	ExitKill       int = 4
	// Go reserves exit code 2 for its own use
)

var (
	ErrTerminated = errors.New("received termination signal")
)
