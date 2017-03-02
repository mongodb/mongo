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

package utils

import (
	"errors"
	"strings"
)

// ErrorGroup collates errors
type ErrorGroup struct {
	Errors []error
}

// Add adds an error to an existing error group
func (e *ErrorGroup) Add(err error) {
	if err != nil {
		e.Errors = append(e.Errors, err)
	}
}

// Finalize returns an error corresponding to the ErrorGroup state. If there's
// no errors in the group, finalize returns nil. If there's only one error,
// Finalize returns that error. Otherwise, Finalize will make a new error
// consisting of the messages from the constituent errors.
func (e *ErrorGroup) Finalize() error {
	if len(e.Errors) == 0 {
		return nil
	}
	if len(e.Errors) == 1 {
		return e.Errors[0]
	}
	msgs := make([]string, 0, len(e.Errors))
	for _, err := range e.Errors {
		msgs = append(msgs, err.Error())
	}
	return errors.New(strings.Join(msgs, "\n"))
}
