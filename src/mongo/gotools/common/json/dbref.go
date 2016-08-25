package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing DBRef and Dbref.
// Adapted from encoding/json/scanner.go.

// stateDB is the state after reading `DB`.
func stateDBR(s *scanner, c int) int {
	if c == 'e' {
		s.step = generateState("DBRef", []byte("f"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal DBRef (expecting 'e')")
}

// stateDb is the state after reading `Db`.
func stateDb(s *scanner, c int) int {
	if c == 'r' {
		s.step = generateState("Dbref", []byte("ef"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal Dbref (expecting 'r')")
}

// Decodes a DBRef literal stored in the underlying byte data into v.
func (d *decodeState) storeDBRef(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args := d.ctorInterface()
	if len(args) != 2 {
		d.error(fmt.Errorf("expected 2 arguments to DBRef constructor, but %v received", len(args)))
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		arg0, ok := args[0].(string)
		if !ok {
			d.error(fmt.Errorf("expected first argument to DBRef to be of type string"))
		}
		arg1 := args[1]
		v.Set(reflect.ValueOf(DBRef{arg0, arg1, ""}))
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", dbRefType, kind))
	}
}

// Returns a DBRef literal from the underlying byte data.
func (d *decodeState) getDBRef() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("DBRef", 2, len(args)); err != nil {
		d.error(err)
	}
	arg0, ok := args[0].(string)
	if !ok {
		d.error(fmt.Errorf("expected string for first argument of DBRef constructor"))
	}
	return DBRef{arg0, args[1], ""}
}
