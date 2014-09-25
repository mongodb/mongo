// Package util implements various utility functions.
package util

import "runtime"
import "os"

//Required for stat1.js: exit code on failure is -1, or 255 for Windows
func ExitFail() {
	failureCode := -1
	if runtime.GOOS == "windows" {
		failureCode = 255
	}
	os.Exit(failureCode)
}
