package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing NumberInt and NumberLong.
// Adapted from encoding/json/scanner.go.

// stateUpperNu is the state after reading `Nu`.
func stateUpperNu(s *scanner, c int) int {
	if c == 'm' {
		s.step = stateUpperNum
		return scanContinue
	}
	return s.error(c, "in literal Number (expecting 'm')")
}

// stateUpperNum is the state after reading `Num`.
func stateUpperNum(s *scanner, c int) int {
	if c == 'b' {
		s.step = stateUpperNumb
		return scanContinue
	}
	return s.error(c, "in literal Number (expecting 'b')")
}

// stateUpperNumb is the state after reading `Numb`.
func stateUpperNumb(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateUpperNumbe
		return scanContinue
	}
	return s.error(c, "in literal Number (expecting 'e')")
}

// stateUpperNumbe is the state after reading `Numbe`.
func stateUpperNumbe(s *scanner, c int) int {
	if c == 'r' {
		s.step = stateUpperNumber
		return scanContinue
	}
	return s.error(c, "in literal Number (expecting 'r')")
}

// stateUpperNumber is the state after reading `Number`.
func stateUpperNumber(s *scanner, c int) int {
	if c == 'I' {
		s.step = stateNumberI
		return scanContinue
	}
	if c == 'L' {
		s.step = stateNumberL
		return scanContinue
	}
	return s.error(c, "in literal NumberInt or NumberLong (expecting 'I' or 'L')")
}

// stateNumberI is the state after reading `NumberI`.
func stateNumberI(s *scanner, c int) int {
	if c == 'n' {
		s.step = stateNumberIn
		return scanContinue
	}
	return s.error(c, "in literal NumberInt (expecting 'n')")
}

// stateNumberIn is the state after reading `NumberIn`.
func stateNumberIn(s *scanner, c int) int {
	if c == 't' {
		s.step = stateConstructor
		return scanContinue
	}
	return s.error(c, "in literal NumberInt (expecting 't')")
}

// stateNumberL is the state after reading `NumberL`.
func stateNumberL(s *scanner, c int) int {
	if c == 'o' {
		s.step = stateNumberLo
		return scanContinue
	}
	return s.error(c, "in literal NumberLong (expecting 'o')")
}

// stateNumberLo is the state after reading `NumberLo`.
func stateNumberLo(s *scanner, c int) int {
	if c == 'n' {
		s.step = stateNumberLon
		return scanContinue
	}
	return s.error(c, "in literal NumberLong (expecting 'n')")
}

// stateNumberLon is the state after reading `NumberLon`.
func stateNumberLon(s *scanner, c int) int {
	if c == 'g' {
		s.step = stateConstructor
		return scanContinue
	}
	return s.error(c, "in literal NumberLong (expecting 'g')")
}

// Decodes a NumberInt literal stored in the underlying byte data into v.
func (d *decodeState) storeNumberInt(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args, err := d.ctor("NumberInt", []reflect.Type{numberIntType})
	if err != nil {
		d.error(err)
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		v.Set(args[0])
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", numberIntType, kind))
	}
}

// Returns a NumberInt literal from the underlying byte data.
func (d *decodeState) getNumberInt() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Prevent d.convertNumber() from parsing the argument as a float64.
	useNumber := d.useNumber
	d.useNumber = true

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("NumberInt", 1, len(args)); err != nil {
		d.error(err)
	}
	arg0, err := args[0].(Number).Int32()
	if err != nil {
		d.error(fmt.Errorf("expected int32 for first argument of NumberInt constructor"))
	}

	d.useNumber = useNumber
	return NumberInt(arg0)
}

// Decodes a NumberLong literal stored in the underlying byte data into v.
func (d *decodeState) storeNumberLong(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args, err := d.ctor("NumberLong", []reflect.Type{numberLongType})
	if err != nil {
		d.error(err)
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		v.Set(args[0])
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", numberLongType, kind))
	}
}

// Returns a NumberLong literal from the underlying byte data.
func (d *decodeState) getNumberLong() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Prevent d.convertNumber() from parsing the argument as a float64.
	useNumber := d.useNumber
	d.useNumber = true

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("NumberLong", 1, len(args)); err != nil {
		d.error(err)
	}
	arg0, err := args[0].(Number).Int64()
	if err != nil {
		d.error(fmt.Errorf("expected int64 for first argument of NumberLong constructor"))
	}

	d.useNumber = useNumber
	return NumberLong(arg0)
}
