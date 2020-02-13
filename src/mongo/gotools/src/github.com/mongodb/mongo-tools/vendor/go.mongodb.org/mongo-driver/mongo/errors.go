// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"bytes"
	"errors"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
)

// ErrUnacknowledgedWrite is returned from functions that have an unacknowledged
// write concern.
var ErrUnacknowledgedWrite = errors.New("unacknowledged write")

// ErrClientDisconnected is returned when a user attempts to call a method on a
// disconnected client
var ErrClientDisconnected = errors.New("client is disconnected")

// ErrNilDocument is returned when a user attempts to pass a nil document or filter
// to a function where the field is required.
var ErrNilDocument = errors.New("document is nil")

// ErrEmptySlice is returned when a user attempts to pass an empty slice as input
// to a function wehere the field is required.
var ErrEmptySlice = errors.New("must provide at least one element in input slice")

func replaceErrors(err error) error {
	if err == topology.ErrTopologyClosed {
		return ErrClientDisconnected
	}
	if de, ok := err.(driver.Error); ok {
		return CommandError{Code: de.Code, Message: de.Message, Labels: de.Labels, Name: de.Name}
	}
	if qe, ok := err.(driver.QueryFailureError); ok {
		// qe.Message is "command failure"
		ce := CommandError{Name: qe.Message}

		dollarErr, err := qe.Response.LookupErr("$err")
		if err == nil {
			ce.Message, _ = dollarErr.StringValueOK()
		}
		code, err := qe.Response.LookupErr("code")
		if err == nil {
			ce.Code, _ = code.Int32OK()
		}

		return ce
	}

	return err
}

// CommandError represents an error in execution of a command against the database.
type CommandError struct {
	Code    int32
	Message string
	Labels  []string
	Name    string
}

// Error implements the error interface.
func (e CommandError) Error() string {
	if e.Name != "" {
		return fmt.Sprintf("(%v) %v", e.Name, e.Message)
	}
	return e.Message
}

// HasErrorLabel returns true if the error contains the specified label.
func (e CommandError) HasErrorLabel(label string) bool {
	if e.Labels != nil {
		for _, l := range e.Labels {
			if l == label {
				return true
			}
		}
	}
	return false
}

// IsMaxTimeMSExpiredError indicates if the error is a MaxTimeMSExpiredError.
func (e CommandError) IsMaxTimeMSExpiredError() bool {
	return e.Code == 50 || e.Name == "MaxTimeMSExpired"
}

// WriteError is a non-write concern failure that occurred as a result of a write
// operation.
type WriteError struct {
	Index   int
	Code    int
	Message string
}

func (we WriteError) Error() string { return we.Message }

// WriteErrors is a group of non-write concern failures that occurred as a result
// of a write operation.
type WriteErrors []WriteError

func (we WriteErrors) Error() string {
	var buf bytes.Buffer
	fmt.Fprint(&buf, "write errors: [")
	for idx, err := range we {
		if idx != 0 {
			fmt.Fprintf(&buf, ", ")
		}
		fmt.Fprintf(&buf, "{%s}", err)
	}
	fmt.Fprint(&buf, "]")
	return buf.String()
}

func writeErrorsFromDriverWriteErrors(errs driver.WriteErrors) WriteErrors {
	wes := make(WriteErrors, 0, len(errs))
	for _, err := range errs {
		wes = append(wes, WriteError{Index: int(err.Index), Code: int(err.Code), Message: err.Message})
	}
	return wes
}

// WriteConcernError is a write concern failure that occurred as a result of a
// write operation.
type WriteConcernError struct {
	Name    string
	Code    int
	Message string
	Details bson.Raw
}

func (wce WriteConcernError) Error() string {
	if wce.Name != "" {
		return fmt.Sprintf("(%v) %v", wce.Name, wce.Message)
	}
	return wce.Message
}

// WriteException is an error for a non-bulk write operation.
type WriteException struct {
	WriteConcernError *WriteConcernError
	WriteErrors       WriteErrors
}

func (mwe WriteException) Error() string {
	var buf bytes.Buffer
	fmt.Fprint(&buf, "multiple write errors: [")
	fmt.Fprintf(&buf, "{%s}, ", mwe.WriteErrors)
	fmt.Fprintf(&buf, "{%s}]", mwe.WriteConcernError)
	return buf.String()
}

func convertDriverWriteConcernError(wce *driver.WriteConcernError) *WriteConcernError {
	if wce == nil {
		return nil
	}

	return &WriteConcernError{Code: int(wce.Code), Message: wce.Message, Details: bson.Raw(wce.Details)}
}

// BulkWriteError is an error for one operation in a bulk write.
type BulkWriteError struct {
	WriteError
	Request WriteModel
}

func (bwe BulkWriteError) Error() string {
	var buf bytes.Buffer
	fmt.Fprintf(&buf, "{%s}", bwe.WriteError)
	return buf.String()
}

// BulkWriteException is an error for a bulk write operation.
type BulkWriteException struct {
	WriteConcernError *WriteConcernError
	WriteErrors       []BulkWriteError
}

func (bwe BulkWriteException) Error() string {
	var buf bytes.Buffer
	fmt.Fprint(&buf, "bulk write error: [")
	fmt.Fprintf(&buf, "{%s}, ", bwe.WriteErrors)
	fmt.Fprintf(&buf, "{%s}]", bwe.WriteConcernError)
	return buf.String()
}

// returnResult is used to determine if a function calling processWriteError should return
// the result or return nil. Since the processWriteError function is used by many different
// methods, both *One and *Many, we need a way to differentiate if the method should return
// the result and the error.
type returnResult int

const (
	rrNone returnResult = 1 << iota // None means do not return the result ever.
	rrOne                           // One means return the result if this was called by a *One method.
	rrMany                          // Many means return the result is this was called by a *Many method.

	rrAll returnResult = rrOne | rrMany // All means always return the result.
)

// processWriteError handles processing the result of a write operation. If the retrunResult matches
// the calling method's type, it should return the result object in addition to the error.
// This function will wrap the errors from other packages and return them as errors from this package.
//
// WriteConcernError will be returned over WriteErrors if both are present.
func processWriteError(err error) (returnResult, error) {
	switch {
	case err == driver.ErrUnacknowledgedWrite:
		return rrAll, ErrUnacknowledgedWrite
	case err != nil:
		switch tt := err.(type) {
		case driver.WriteCommandError:
			return rrMany, WriteException{
				WriteConcernError: convertDriverWriteConcernError(tt.WriteConcernError),
				WriteErrors:       writeErrorsFromDriverWriteErrors(tt.WriteErrors),
			}
		default:
			return rrNone, replaceErrors(err)
		}
	default:
		return rrAll, nil
	}
}
