// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"errors"
	"fmt"

	"strings"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/network/result"
)

var (
	// ErrUnknownCommandFailure occurs when a command fails for an unknown reason.
	ErrUnknownCommandFailure = errors.New("unknown command failure")
	// ErrNoCommandResponse occurs when the server sent no response document to a command.
	ErrNoCommandResponse = errors.New("no command response document")
	// ErrMultiDocCommandResponse occurs when the server sent multiple documents in response to a command.
	ErrMultiDocCommandResponse = errors.New("command returned multiple documents")
	// ErrNoDocCommandResponse occurs when the server indicated a response existed, but none was found.
	ErrNoDocCommandResponse = errors.New("command returned no documents")
	// ErrDocumentTooLarge occurs when a document that is larger than the maximum size accepted by a
	// server is passed to an insert command.
	ErrDocumentTooLarge = errors.New("an inserted document is too large")
	// ErrNonPrimaryRP occurs when a nonprimary read preference is used with a transaction.
	ErrNonPrimaryRP = errors.New("read preference in a transaction must be primary")
	// UnknownTransactionCommitResult is an error label for unknown transaction commit results.
	UnknownTransactionCommitResult = "UnknownTransactionCommitResult"
	// TransientTransactionError is an error label for transient errors with transactions.
	TransientTransactionError = "TransientTransactionError"
	// NetworkError is an error label for network errors.
	NetworkError = "NetworkError"
	// ReplyDocumentMismatch is an error label for OP_QUERY field mismatch errors.
	ReplyDocumentMismatch = "malformed OP_REPLY: NumberReturned does not match number of documents returned"
)

var retryableCodes = []int32{11600, 11602, 10107, 13435, 13436, 189, 91, 7, 6, 89, 9001}

// QueryFailureError is an error representing a command failure as a document.
type QueryFailureError struct {
	Message  string
	Response bson.Raw
}

// Error implements the error interface.
func (e QueryFailureError) Error() string {
	return fmt.Sprintf("%s: %v", e.Message, e.Response)
}

// ResponseError is an error parsing the response to a command.
type ResponseError struct {
	Message string
	Wrapped error
}

// NewCommandResponseError creates a CommandResponseError.
func NewCommandResponseError(msg string, err error) ResponseError {
	return ResponseError{Message: msg, Wrapped: err}
}

// Error implements the error interface.
func (e ResponseError) Error() string {
	if e.Wrapped != nil {
		return fmt.Sprintf("%s: %s", e.Message, e.Wrapped)
	}
	return fmt.Sprintf("%s", e.Message)
}

// Error is a command execution error from the database.
type Error struct {
	Code    int32
	Message string
	Labels  []string
	Name    string
}

// Error implements the error interface.
func (e Error) Error() string {
	if e.Name != "" {
		return fmt.Sprintf("(%v) %v", e.Name, e.Message)
	}
	return e.Message
}

// HasErrorLabel returns true if the error contains the specified label.
func (e Error) HasErrorLabel(label string) bool {
	if e.Labels != nil {
		for _, l := range e.Labels {
			if l == label {
				return true
			}
		}
	}
	return false
}

// Retryable returns true if the error is retryable
func (e Error) Retryable() bool {
	for _, label := range e.Labels {
		if label == NetworkError {
			return true
		}
	}
	for _, code := range retryableCodes {
		if e.Code == code {
			return true
		}
	}
	if strings.Contains(e.Message, "not master") || strings.Contains(e.Message, "node is recovering") {
		return true
	}

	return false
}

// IsWriteConcernErrorRetryable returns true if the write concern error is retryable.
func IsWriteConcernErrorRetryable(wce *result.WriteConcernError) bool {
	for _, code := range retryableCodes {
		if int32(wce.Code) == code {
			return true
		}
	}
	if strings.Contains(wce.ErrMsg, "not master") || strings.Contains(wce.ErrMsg, "node is recovering") {
		return true
	}

	return false
}

// IsNotFound indicates if the error is from a namespace not being found.
func IsNotFound(err error) bool {
	e, ok := err.(Error)
	// need message check because legacy servers don't include the error code
	return ok && (e.Code == 26 || e.Message == "ns not found")
}
