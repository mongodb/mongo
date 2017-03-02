// +build linux darwin freebsd netbsd openbsd

package gopass

import (
	"io"
	"syscall"

	"golang.org/x/crypto/ssh/terminal"
)

const lineEnding = "\n"

func getch() (byte, error) {
	if oldState, err := terminal.MakeRaw(0); err != nil {
		return 0, err
	} else {
		defer terminal.Restore(0, oldState)
	}

	var buf [1]byte
	if n, err := syscall.Read(0, buf[:]); n == 0 || err != nil {
		if err != nil {
			return 0, err
		}
		return 0, io.EOF
	}
	return buf[0], nil
}
