// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package connection

const defaultPort = "27017"

// Addr is a network address. It can be either an IP address or a DNS name.
type Addr string

// Network is the network protcol for this address. In most cases this will be "tcp" or "unix".
func (Addr) Network() string { return "" }

// String is the canonical version of this address, e.g. localhost:27017, 1.2.3.4:27017, example.com:27017
func (Addr) String() string { return "" }

// Canonicalize creates a canonicalized address.
func (Addr) Canonicalize() Addr { return Addr("") }
