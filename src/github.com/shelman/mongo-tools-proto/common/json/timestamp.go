package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing Timestamp.
// Adapted from encoding/json/scanner.go.

// stateUpperT is the state after reading `T`.
func stateUpperT(s *scanner, c int) int {
	if c == 'i' {
		s.step = generateState("Timestamp", []byte("mestamp"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal Timestamp (expecting 'i')")
}

// Decodes a Timestamp literal stored in the underlying byte data into v.
func (d *decodeState) storeTimestamp(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args, err := d.ctor("Timestamp", []reflect.Type{uint32Type, uint32Type})
	if err != nil {
		d.error(err)
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		arg0 := uint32(args[0].Uint())
		arg1 := uint32(args[1].Uint())
		v.Set(reflect.ValueOf(Timestamp{arg0, arg1}))
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", timestampType, kind))
	}
}

// Returns a Timestamp literal from the underlying byte data.
func (d *decodeState) getTimestamp() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Prevent d.convertNumber() from parsing the arguments as float64s.
	useNumber := d.useNumber
	d.useNumber = true

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("Timestamp", 2, len(args)); err != nil {
		d.error(err)
	}
	arg0, err := args[0].(Number).Uint32()
	if err != nil {
		d.error(fmt.Errorf("expected uint32 for first argument of Timestamp constructor"))
	}
	arg1, err := args[1].(Number).Uint32()
	if err != nil {
		d.error(fmt.Errorf("expected uint32 for second argument of Timestamp constructor"))
	}

	d.useNumber = useNumber
	return Timestamp{arg0, arg1}
}
