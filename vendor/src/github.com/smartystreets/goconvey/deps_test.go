// The purpose of this file is to make sure the dependencies are pulled in when
// `go get -t` is invoked for the first time. Because it is in a *_test.go file
// it prevents all of the flags from dependencies from leaking into the goconvey
// binary.

package main

import (
	_ "github.com/jacobsa/oglematchers"
	_ "github.com/jacobsa/ogletest"
)
