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
		s.step = generateState("Number", []byte("ber"), stateUpperNumber)
		return scanContinue
	}
	return s.error(c, "in literal Number (expecting 'm')")
}

// stateUpperNumber is the state after reading `Number`.
func stateUpperNumber(s *scanner, c int) int {
	if c == 'I' {
		s.step = generateState("NumberInt", []byte("nt"), stateConstructor)
		return scanContinue
	}
	if c == 'L' {
		s.step = generateState("NumberLong", []byte("ong"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal NumberInt or NumberLong (expecting 'I' or 'L')")
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
	var number Number
	switch v := args[0].(type) {
	case Number:
		number = v
	case string:
		number = Number(v)
	default:
		d.error(fmt.Errorf("expected int32 for first argument of NumberInt constructor, got %T (value was %v)", v, v))
	}

	d.useNumber = useNumber
	arg0, err := number.Int32()
	if err != nil {
		d.error(fmt.Errorf("expected int32 for first argument of NumberInt constructor, got %T (value was %v)", number, number))
	}
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
	var number Number
	switch v := args[0].(type) {
	case Number:
		number = v
	case string:
		number = Number(v)

	default:
		d.error(fmt.Errorf("expected int64 for first argument of NumberLong constructor, got %T (value was %v)", v, v))
	}

	d.useNumber = useNumber
	arg0, err := number.Int64()
	if err != nil {
		d.error(fmt.Errorf("expected int64 for first argument of NumberLong constructor, got %T (value was %v)", number, number))
	}
	return NumberLong(arg0)
}
