// +build !windows

// Package sysutil contains system-specific utilities.
package sysutil

import "golang.org/x/sys/unix"

// RlimitStack reports the current stack size limit in bytes.
func RlimitStack() (cur uint64, err error) {
	var r unix.Rlimit
	err = unix.Getrlimit(unix.RLIMIT_STACK, &r)
	return uint64(r.Cur), err // Type conversion because Cur is one of uint64, int64 depending on unix flavor.
}
