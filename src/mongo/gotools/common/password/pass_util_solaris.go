// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package password

import (
	"os"
	"syscall"
	"unsafe"
)

// This file is a mess based primarily on
// 		"github.com/howeyc/gopass"
// 		"golang.org/x/crypto/ssh/terminal"
// with extra unistd.h ripped from solaris on amd64
//
// TODO: get some of these changes merged into the above two packages

// ioctl constants -- not defined in solaris syscall pkg
const (
	SYS_IOCTL = 54
	TCGETS    = 21517
	TCSETS    = 21518
	ttyfd     = 0 //STDIN
)

// getTermios reads the current termios settings into the
// given termios struct.
func getTermios(term *syscall.Termios) error {
	_, _, errno := syscall.Syscall(SYS_IOCTL,
		uintptr(ttyfd), uintptr(TCGETS),
		uintptr(unsafe.Pointer(term)))
	if errno != 0 {
		return os.NewSyscallError("SYS_IOCTL", errno)
	}
	return nil
}

// setTermios applies the supplied termios settings
func setTermios(term *syscall.Termios) error {
	_, _, errno := syscall.Syscall(SYS_IOCTL,
		uintptr(ttyfd), uintptr(TCSETS),
		uintptr(unsafe.Pointer(term)))
	if errno != 0 {
		return os.NewSyscallError("SYS_IOCTL", errno)
	}
	return nil
}

// setRaw puts the terminal into "raw" mode, which takes
// in all key presses and does not echo them.
func setRaw(term syscall.Termios) error {
	termCopy := term
	termCopy.Iflag &^= syscall.ISTRIP | syscall.INLCR |
		syscall.ICRNL | syscall.IGNCR | syscall.IXON | syscall.IXOFF
	termCopy.Lflag &^= syscall.ECHO | syscall.ICANON | syscall.ISIG
	return setTermios(&termCopy)
}

// isTerminal checks if we are reading from a terminal (instead of a pipe).
func IsTerminal() bool {
	var termios syscall.Termios
	_, _, errno := syscall.Syscall(SYS_IOCTL,
		uintptr(ttyfd), TCGETS,
		uintptr(unsafe.Pointer(&termios)))
	return errno == 0
}

// readChar safely gets one byte from stdin
func readChar() byte {
	var originalTerm syscall.Termios
	if err := getTermios(&originalTerm); err != nil {
		panic(err) // should not happen on amd64 solaris (untested on sparc)
	}
	if err := setRaw(originalTerm); err != nil {
		panic(err)
	}
	defer func() {
		// make sure we return the termios back to normal
		if err := setTermios(&originalTerm); err != nil {
			panic(err)
		}
	}()

	// read a single byte then reset the terminal state
	var singleChar [1]byte
	if n, err := syscall.Read(ttyfd, singleChar[:]); n == 0 || err != nil {
		panic(err)
	}
	return singleChar[0]
}

// get password from terminal
func GetPass() string {
	// keep reading in characters until we hit a stopping point
	pass := []byte{}
	for {
		ch := readChar()
		if ch == backspaceKey || ch == deleteKey {
			if len(pass) > 0 {
				pass = pass[:len(pass)-1]
			}
		} else if ch == carriageReturnKey || ch == newLineKey || ch == eotKey || ch == eofKey {
			break
		} else if ch != 0 {
			pass = append(pass, ch)
		}
	}
	return string(pass)
}
