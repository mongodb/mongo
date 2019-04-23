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

// Transition functions for recognizing ObjectId.
// Adapted from encoding/json/scanner.go.

// stateO is the state after reading `O`.
func stateO(s *scanner, c int) int {
	if c == 'b' {
		s.step = generateState("ObjectId", []byte("jectId"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal ObjectId (expecting 'b')")
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
