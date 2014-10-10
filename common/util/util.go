// Package util implements various utility functions.
package util

import "os"

func ExitFail() {
	os.Exit(-1)
}
