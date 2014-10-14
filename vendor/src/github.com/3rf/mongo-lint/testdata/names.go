// Test for name linting.

// Package pkg_with_underscores ...
package pkg_with_underscores // MATCH /underscore.*package name/

var var_name int // MATCH /underscore.*var.*var_name/

type t_wow struct { // MATCH /underscore.*type.*t_wow/
	x_damn int      // MATCH /underscore.*field.*x_damn/
	Url    *url.URL // MATCH /struct field.*Url.*URL/
}

const fooId = "blah" // MATCH /fooId.*fooID/

func f_it() { // MATCH /underscore.*func.*f_it/
	more_underscore := 4                 // MATCH /underscore.*var.*more_underscore/
	if isEof := (err == io.EOF); isEof { // MATCH /var.*isEof.*isEOF/
		more_underscore = 7 // should be okay
	}

	x := foo_proto.Blah{} // should be okay

	for _, theIp := range ips { // MATCH /range var.*theIp.*theIP/
	}

	switch myJson := g(); { // MATCH /var.*myJson.*myJSON/
	}
	switch tApi := x.(type) { // MATCH /var.*tApi.*tAPI/
	}

	select {
	case qId := <-c: // MATCH /var.*qId.*qID/
	}
}

// Common styles in other languages that don't belong in Go.
const (
	CPP_CONST   = 1 // MATCH /ALL_CAPS.*CamelCase/
	kLeadingKay = 2 // MATCH /k.*leadingKay/

	HTML  = 3 // okay; no underscore
	X509B = 4 // ditto
)

func f(bad_name int)                    {} // MATCH /underscore.*func parameter.*bad_name/
func g() (no_way int)                   {} // MATCH /underscore.*func result.*no_way/
func (t *t_wow) f(more_under string)    {} // MATCH /underscore.*method parameter.*more_under/
func (t *t_wow) g() (still_more string) {} // MATCH /underscore.*method result.*still_more/

type i interface {
	CheckHtml() string // okay; interface method names are often constrained by the concrete types' method names

	F(foo_bar int) // MATCH /foo_bar.*fooBar/
}
