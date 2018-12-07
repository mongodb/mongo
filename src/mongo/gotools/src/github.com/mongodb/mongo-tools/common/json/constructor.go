package json

import (
	"fmt"
	"reflect"
)

const CtorNumArgsErrorf = "expected %v argument%v to %v constructor, but %v received"

// Transition functions for recognizing object constructors.
// Adapted from encoding/json/scanner.go.

// stateConstructor is the state after reading a constructor name.
func stateConstructor(s *scanner, c int) int {
	if c <= ' ' && isSpace(rune(c)) {
		return scanSkipSpace
	}
	if c == '(' {
		s.step = stateBeginCtorOrEmpty
		s.pushParseState(parseCtorArg)
		return scanBeginCtor
	}
	return s.error(c, "expected '('")
}

// stateBeginCtorOrEmpty is the state after reading `(`.
func stateBeginCtorOrEmpty(s *scanner, c int) int {
	if c <= ' ' && isSpace(rune(c)) {
		return scanSkipSpace
	}
	if c == ')' {
		return stateEndValue(s, c)
	}
	return stateBeginValue(s, c)
}

// ctor consumes a constructor from d.data[d.off-1:], given a type specification t.
// the first byte of the constructor ('(') has been read already.
func (d *decodeState) ctor(name string, t []reflect.Type) ([]reflect.Value, error) {
	result := make([]reflect.Value, 0, len(t))

	i := 0
	for {
		// Look ahead for ) - can only happen on first iteration.
		op := d.scanWhile(scanSkipSpace)
		if op == scanEndCtor {
			break
		}

		// Back up so d.value can have the byte we just read.
		d.off--
		d.scan.undo(op)

		if i < len(t) {
			v := reflect.New(t[i]).Elem()

			// Get argument of constructor
			d.value(v)

			result = append(result, v)
			i++
		}

		// Next token must be , or ).
		op = d.scanWhile(scanSkipSpace)
		if op == scanEndCtor {
			break
		}
		if op != scanCtorArg {
			d.error(errPhase)
		}
	}

	return result, ctorNumArgsMismatch(name, len(t), i)
}

// ctorInterface is like ctor but returns []interface{}.
func (d *decodeState) ctorInterface() []interface{} {
	var v = make([]interface{}, 0)
	for {
		// Look ahead for ) - can only happen on first iteration.
		op := d.scanWhile(scanSkipSpace)
		if op == scanEndCtor {
			break
		}

		// Back up so d.value can have the byte we just read.
		d.off--
		d.scan.undo(op)

		v = append(v, d.valueInterface(false))

		// Next token must be , or ).
		op = d.scanWhile(scanSkipSpace)
		if op == scanEndCtor {
			break
		}
		if op != scanCtorArg {
			d.error(errPhase)
		}
	}
	return v
}

// Returns a descriptive error message if the number of arguments given
// to the constructor do not match what is expected.
func ctorNumArgsMismatch(name string, expected, actual int) error {
	if expected == actual {
		return nil
	}

	quantifier := ""
	if expected > 1 {
		quantifier = "s"
	}
	return fmt.Errorf(CtorNumArgsErrorf, expected, quantifier, name, actual)
}
