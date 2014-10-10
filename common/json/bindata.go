package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing BinData.
// Adapted from encoding/json/scanner.go.

// stateB is the state after reading `B`.
func stateB(s *scanner, c int) int {
	if c == 'i' {
		s.step = generateState("BinData", []byte("nData"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal BinData (expecting 'i')")
}

// Decodes a BinData literal stored in the underlying byte data into v.
func (d *decodeState) storeBinData(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args, err := d.ctor("BinData", []reflect.Type{byteType, stringType})
	if err != nil {
		d.error(err)
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		arg0 := byte(args[0].Uint())
		arg1 := args[1].String()
		v.Set(reflect.ValueOf(BinData{arg0, arg1}))
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", binDataType, kind))
	}
}

// Returns a BinData literal from the underlying byte data.
func (d *decodeState) getBinData() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Prevent d.convertNumber() from parsing the argument as a float64.
	useNumber := d.useNumber
	d.useNumber = true

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("BinData", 2, len(args)); err != nil {
		d.error(err)
	}
	arg0, err := args[0].(Number).Uint8()
	if err != nil {
		d.error(fmt.Errorf("expected byte for first argument of BinData constructor"))
	}
	arg1, ok := args[1].(string)
	if !ok {
		d.error(fmt.Errorf("expected string for second argument of BinData constructor"))
	}

	d.useNumber = useNumber
	return BinData{arg0, arg1}
}
