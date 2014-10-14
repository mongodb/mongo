// Test for not using fmt.Errorf.

// Package foo ...
package foo

import (
	"errors"
	"fmt"
)

func f(x int) error {
	if x > 10 {
		return errors.New(fmt.Sprintf("something %d", x)) // MATCH /should replace.*errors\.New\(fmt\.Sprintf\(\.\.\.\)\).*fmt\.Errorf\(\.\.\.\)/
	}
	if x > 5 {
		return errors.New(g("blah")) // ok
	}
	if x > 4 {
		return errors.New("something else") // ok
	}
	return nil
}
