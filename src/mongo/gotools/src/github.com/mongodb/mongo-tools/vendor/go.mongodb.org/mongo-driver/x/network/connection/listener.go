// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package connection

// Listener is a generic mongodb network protocol listener. It can return connections
// that speak the mongodb wire protocol.
//
// Multiple goroutines may invoke methods on a Listener simultaneously.
//
// TODO(GODRIVER-270): Implement this.
type Listener interface {
	// Accept waits for and returns the next Connection to the listener.
	Accept() (Connection, error)

	// Close closes the listener.
	Close() error

	// Addr returns the listener's network address.
	Addr() Addr
}

// Listen creates a new listener on the provided network and address.
func Listen(network, address string) (Listener, error) { return nil, nil }
