// +build !solaris

package password

import (
	"github.com/howeyc/gopass"
	"golang.org/x/crypto/ssh/terminal"
	"syscall"
)

// This file contains all the calls needed to properly
// handle password input from stdin/terminal on all
// operating systems that aren't solaris

func IsTerminal() bool {
	return terminal.IsTerminal(int(syscall.Stdin))
}

func GetPass() string {
	pass, _ := gopass.GetPasswd()
	return string(pass)
}
