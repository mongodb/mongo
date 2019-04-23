// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package connection

import "fmt"

// Error represents a connection error.
type Error struct {
	ConnectionID string
	Wrapped      error

	message string
}

// Error implements the error interface.
func (e Error) Error() string {
	if e.Wrapped != nil {
		return fmt.Sprintf("connection(%s) %s: %s", e.ConnectionID, e.message, e.Wrapped.Error())
	}
	return fmt.Sprintf("connection(%s) %s", e.ConnectionID, e.message)
}

// NetworkError represents an error that occurred while reading from or writing
// to a network socket.
type NetworkError struct {
	ConnectionID string
	Wrapped      error
}

func (ne NetworkError) Error() string {
	return fmt.Sprintf("connection(%s): %s", ne.ConnectionID, ne.Wrapped.Error())
}

// PoolError is an error returned from a Pool method.
type PoolError string

func (pe PoolError) Error() string { return string(pe) }
