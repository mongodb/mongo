// +build js

package strings

import (
	"unicode/utf8"

	"github.com/gopherjs/gopherjs/js"
)

func IndexByte(s string, c byte) int {
	return js.InternalObject(s).Call("indexOf", js.Global.Get("String").Call("fromCharCode", c)).Int()
}

func Index(s, sep string) int {
	return js.InternalObject(s).Call("indexOf", js.InternalObject(sep)).Int()
}

func LastIndex(s, sep string) int {
	return js.InternalObject(s).Call("lastIndexOf", js.InternalObject(sep)).Int()
}

func Count(s, sep string) int {
	n := 0
	// special cases
	switch {
	case len(sep) == 0:
		return utf8.RuneCountInString(s) + 1
	case len(sep) > len(s):
		return 0
	case len(sep) == len(s):
		if sep == s {
			return 1
		}
		return 0
	}

	for {
		pos := Index(s, sep)
		if pos == -1 {
			break
		}
		n++
		s = s[pos+len(sep):]
	}
	return n
}

func (b *Builder) String() string {
	// Upstream Builder.String relies on package unsafe. We can't do that.
	// TODO: It's possible that the entire strings.Builder API can be implemented
	//       more efficiently for GOARCH=js specifically (avoid using []byte, instead
	//       use a String directly; or some JavaScript string builder API if one exists).
	//       But this is more work, defer doing it until there's a need shown via profiling,
	//       and there are benchmarks available (see https://github.com/golang/go/issues/18990#issuecomment-352068533).
	return string(b.buf)
}

func (b *Builder) copyCheck() {
	if b.addr == nil {
		// Upstream copyCheck uses noescape, which performs unsafe.Pointer manipulation.
		// We can't do that, so skip it. See https://github.com/golang/go/commit/484586c81a0196e42ac52f651bc56017ca454280.
		b.addr = b
	} else if b.addr != b {
		panic("strings: illegal use of non-zero Builder copied by value")
	}
}
