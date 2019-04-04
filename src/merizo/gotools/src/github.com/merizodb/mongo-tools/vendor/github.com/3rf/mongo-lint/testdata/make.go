// Test for pointless make() calls.

// Package pkg ...
package pkg

func f() {
	x := make([]T, 0)               // MATCH /var x \[\]T/
	y := make([]somepkg.Foo_Bar, 0) // MATCH /var y \[\]somepkg.Foo_Bar/
	z = make([]T, 0)                // ok, because we don't know where z is declared
}
