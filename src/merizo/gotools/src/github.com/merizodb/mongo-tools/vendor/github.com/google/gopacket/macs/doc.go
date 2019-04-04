// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// Package macs provides an in-memory mapping of all valid Ethernet MAC address
// prefixes to their associated organization.
//
// The ValidMACPrefixMap map maps 3-byte prefixes to organization strings.  It
// can be updated using 'go run gen.go' in this directory.
package macs
