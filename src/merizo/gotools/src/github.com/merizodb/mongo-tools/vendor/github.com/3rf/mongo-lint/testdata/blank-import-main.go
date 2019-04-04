// Test that blank imports in package main are not flagged.
// OK

// Binary foo ...
package main

import _ "fmt"

import (
	"os"
	_ "path"
)
