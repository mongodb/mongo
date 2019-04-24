// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

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
