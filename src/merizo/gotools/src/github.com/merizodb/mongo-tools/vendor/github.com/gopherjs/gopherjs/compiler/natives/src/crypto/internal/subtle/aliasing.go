// +build js

package subtle

import "github.com/gopherjs/gopherjs/js"

// AnyOverlap reports whether x and y share memory at any (not necessarily
// corresponding) index. The memory beyond the slice length is ignored.
func AnyOverlap(x, y []byte) bool {
	// GopherJS: We can't rely on pointer arithmetic, so use GopherJS slice internals.
	return len(x) > 0 && len(y) > 0 &&
		js.InternalObject(x).Get("$array") == js.InternalObject(y).Get("$array") &&
		js.InternalObject(x).Get("$offset").Int() <= js.InternalObject(y).Get("$offset").Int()+len(y)-1 &&
		js.InternalObject(y).Get("$offset").Int() <= js.InternalObject(x).Get("$offset").Int()+len(x)-1
}
