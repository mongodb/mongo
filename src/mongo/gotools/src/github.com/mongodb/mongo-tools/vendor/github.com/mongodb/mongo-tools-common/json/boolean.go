// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing Boolean.
// Adapted from encoding/json/scanner.go.

// stateBo is the state after reading `Bo`.
func stateBo(s *scanner, c int) int {
	if c == 'o' {
		s.step = generateState("Boolean", []byte("lean"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal Boolean (expecting 'o')")
}

// Decodes a Boolean literal stored in the underlying byte data into v.
func (d *decodeState) storeBoolean(v reflect.Value) {
	res := d.getBoolean()
	switch kind := v.Kind(); kind {
	case reflect.Interface, reflect.Bool:
		v.Set(reflect.ValueOf(res))
	default:
		d.error(fmt.Errorf("cannot store bool value into %v type", kind))
	}
}

// Returns a Boolean literal from the underlying byte data.
func (d *decodeState) getBoolean() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Prevent d.convertNumber() from parsing the argument as a float64.
	useNumber := d.useNumber
	d.useNumber = true

	args := d.ctorInterface()
	if len(args) == 0 {
		return false
	}

	// Ignore all but the first argument.
	switch v := args[0].(type) {
	case bool:
		return v
	case Number:
		d.useNumber = useNumber

		// First try Int64 so hex numbers work, then if that fails try Float64.
		num, err := v.Int64()
		if err == nil {
			return (num != 0)
		}

		numF, err := v.Float64()
		if err != nil {
			d.error(fmt.Errorf("expected float64 for numeric argument of Boolean constructor, got err: %v", err))
		}
		return (numF != 0)
	case string:
		return (v != "")
	case Undefined, nil:
		return false
	// Parameter values of any other types should yield true.
	default:
		return true
	}
}
