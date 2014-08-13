package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing DBRef and Dbref.
// Adapted from encoding/json/scanner.go.

// stateDB is the state after reading `DB`.
func stateDB(s *scanner, c int) int {
	if c == 'R' {
		s.step = stateDBR
		return scanContinue
	}
	return s.error(c, "in literal DBRef (expecting 'R')")
}

// stateDBR is the state after reading `DBR`.
func stateDBR(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateDBRe
		return scanContinue
	}
	return s.error(c, "in literal DBRef (expecting 'e')")
}

// stateDBRe is the state after reading `DBRe`.
func stateDBRe(s *scanner, c int) int {
	if c == 'f' {
		s.step = stateConstructor
		return scanContinue
	}
	return s.error(c, "in literal DBRef (expecting 'f')")
}

// stateDb is the state after reading `Db`.
func stateDb(s *scanner, c int) int {
	if c == 'r' {
		s.step = stateDbr
		return scanContinue
	}
	return s.error(c, "in literal Dbref (expecting 'r')")
}

// stateDbr is the state after reading `Dbr`.
func stateDbr(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateDBRe
		return scanContinue
	}
	return s.error(c, "in literal Dbref (expecting 'e')")
}

// stateDbre is the state after reading `Dbre`.
func stateDbre(s *scanner, c int) int {
	if c == 'f' {
		s.step = stateConstructor
		return scanContinue
	}
	return s.error(c, "in literal Dbref (expecting 'f')")
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
		v.Set(reflect.ValueOf(DBRef{arg0, arg1}))
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
	return DBRef{arg0, args[1]}
}
