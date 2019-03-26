// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build linux

package afpacket

import (
	"unsafe"

	"golang.org/x/sys/unix"
)

// setsockopt provides access to the setsockopt syscall.
func setsockopt(fd, level, name int, val unsafe.Pointer, vallen uintptr) error {
	_, _, errno := unix.Syscall6(
		unix.SYS_SETSOCKOPT,
		uintptr(fd),
		uintptr(level),
		uintptr(name),
		uintptr(val),
		vallen,
		0,
	)
	if errno != 0 {
		return error(errno)
	}

	return nil
}

// getsockopt provides access to the getsockopt syscall.
func getsockopt(fd, level, name int, val unsafe.Pointer, vallen uintptr) error {
	_, _, errno := unix.Syscall6(
		unix.SYS_GETSOCKOPT,
		uintptr(fd),
		uintptr(level),
		uintptr(name),
		uintptr(val),
		vallen,
		0,
	)
	if errno != 0 {
		return error(errno)
	}

	return nil
}

// htons converts a short (uint16) from host-to-network byte order.
// Thanks to mikioh for this neat trick:
// https://github.com/mikioh/-stdyng/blob/master/afpacket.go
func htons(i uint16) uint16 {
	return (i<<8)&0xff00 | i>>8
}
