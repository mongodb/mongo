package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing ObjectId.
// Adapted from encoding/json/scanner.go.

// stateO is the state after reading `O`.
func stateO(s *scanner, c int) int {
	if c == 'b' {
		s.step = stateOb
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 'b')")
}

// stateOb is the state after reading `Ob`.
func stateOb(s *scanner, c int) int {
	if c == 'j' {
		s.step = stateObj
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 'j')")
}

// stateObj is the state after reading `Obj`.
func stateObj(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateObje
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 'e')")
}

// stateObje is the state after reading `Obje`.
func stateObje(s *scanner, c int) int {
	if c == 'c' {
		s.step = stateObjec
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 'c')")
}

// stateObjec is the state after reading `Objec`.
func stateObjec(s *scanner, c int) int {
	if c == 't' {
		s.step = stateObject
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 't')")
}

// stateObject is the state after reading `Object`.
func stateObject(s *scanner, c int) int {
	if c == 'I' {
		s.step = stateObjectI
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 'I')")
}

// stateObjectI is the state after reading `ObjectI`.
func stateObjectI(s *scanner, c int) int {
	if c == 'd' {
		s.step = stateConstructor
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 'd')")
}

// Decodes an ObjectId literal stored in the underlying byte data into v.
func (d *decodeState) storeObjectId(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args, err := d.ctor("ObjectId", []reflect.Type{objectIdType})
	if err != nil {
		d.error(err)
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		v.Set(args[0])
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", objectIdType, kind))
	}
}

// Returns an ObjectId literal from the underlying byte data.
func (d *decodeState) getObjectId() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("ObjectId", 1, len(args)); err != nil {
		d.error(err)
	}
	arg0, ok := args[0].(string)
	if !ok {
		d.error(fmt.Errorf("expected string for first argument of ObjectId constructor"))
	}
	return ObjectId(arg0)
}
