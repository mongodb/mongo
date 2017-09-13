// Test for naming errors.

// Package foo ...
package foo

import (
	"errors"
	"fmt"
)

var unexp = errors.New("some unexported error") // MATCH /error var.*unexp.*errFoo/

// Exp ...
var Exp = errors.New("some exported error") // MATCH /error var.*Exp.*ErrFoo/

var (
	e1 = fmt.Errorf("blah %d", 4) // MATCH /error var.*e1.*errFoo/
	// E2 ...
	E2 = fmt.Errorf("blah %d", 5) // MATCH /error var.*E2.*ErrFoo/
)

func f() {
	var whatever = errors.New("ok") // ok
}

// Check for the error strings themselves.

func g(x int) error {
	if x < 1 {
		return fmt.Errorf("This %d is too low", x) // MATCH /error strings.*not be capitalized/
	} else if x == 0 {
		return fmt.Errorf("XML time") // ok
	}
	return errors.New(`too much stuff.`) // MATCH /error strings.*not end with punctuation/
}
