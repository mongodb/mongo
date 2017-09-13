// Test that blank imports in library packages are flagged.

// Package foo ...
package foo

// The instructions need to go before the imports below so they will not be
// mistaken for documentation.

/* MATCH /blank import/ */ import _ "encoding/json"

import (
	"fmt"
	/* MATCH /blank import/ */ _ "os"

	/* MATCH /blank import/ */ _ "net/http"
	_ "path"
)

import _ "encoding/base64" // Don't gripe about this

import (
	// Don't gripe about these next two lines.
	_ "compress/zlib"
	_ "syscall"

	/* MATCH /blank import/ */ _ "path/filepath"
)

import (
	"go/ast"
	_ "go/scanner" // Don't gripe about this or the following line.
	_ "go/token"
)
