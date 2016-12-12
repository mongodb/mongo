// Test that blank imports in test packages are not flagged.
// OK

// Package foo ...
package foo

// These are essentially the same imports as in the "library" package, but
// these should not trigger the warning because this is a test.

import _ "encoding/json"

import (
	"fmt"
	"testing"

	_ "os"

	_ "net/http"
	_ "path"
)
