// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build !linux

// Package routing is currently only supported in Linux, but the build system requires a valid go file for all architectures.

package routing

func New() (Router, error) {
	panic("router only implemented in linux")
}
