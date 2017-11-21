// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/util"
	"reflect"
)

// Transition functions for recognizing Date.
// Adapted from encoding/json/scanner.go.

// stateDa is the state after reading `Da`.
func stateDa(s *scanner, c int) int {
	if c == 't' {
		s.step = stateDat
		return scanContinue
	}
	return s.error(c, "in literal Date (expecting 't')")
}

// stateDat is the state after reading `Dat`.
func stateDat(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateConstructor
		return scanContinue
	}
	return s.error(c, "in literal Date (expecting 'e')")
}

// Decodes a Date literal stored in the underlying byte data into v.
func (d *decodeState) storeDate(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}
	args, err := d.ctor("Date", []reflect.Type{dateType})
	if err != nil {
		d.error(err)
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		v.Set(args[0])
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", dateType, kind))
	}
}

// Returns a Date literal from the underlying byte data.
func (d *decodeState) getDate() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Prevent d.convertNumber() from parsing the argument as a float64.
	useNumber := d.useNumber
	d.useNumber = true

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("Date", 1, len(args)); err != nil {
		d.error(err)
	}
	arg0num, isNumber := args[0].(Number)
	if !isNumber {
		// validate the date format of the string
		_, err := util.FormatDate(args[0].(string))
		if err != nil {
			d.error(fmt.Errorf("unexpected ISODate format"))
		}
		d.useNumber = useNumber
		return ISODate(args[0].(string))
	}
	arg0, err := arg0num.Int64()
	if err != nil {
		d.error(fmt.Errorf("expected int64 for first argument of Date constructor"))
	}

	d.useNumber = useNumber
	return Date(arg0)
}
