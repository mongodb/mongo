// Test of return+else warning; should not trigger on multi-branch if/else.
// OK

// Package pkg ...
package pkg

import "log"

func f(x int) bool {
	if x == 0 {
		log.Print("x is zero")
	} else if x > 0 {
		return true
	} else {
		log.Printf("non-positive x: %d", x)
	}
	return false
}
