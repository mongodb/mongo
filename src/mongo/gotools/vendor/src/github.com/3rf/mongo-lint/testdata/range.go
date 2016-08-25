// Test for range construction.

// Package foo ...
package foo

func f() {
	// with :=
	for x, _ := range m { // MATCH /should omit 2nd value.*range.*equivalent.*for x := range/
	}
	// with =
	for y, _ = range m { // MATCH /should omit 2nd value.*range.*equivalent.*for y = range/
	}

	// all OK:
	for x := range m {
	}
	for x, y := range m {
	}
	for _, y := range m {
	}
	for x = range m {
	}
	for x, y = range m {
	}
	for _, y = range m {
	}
}
