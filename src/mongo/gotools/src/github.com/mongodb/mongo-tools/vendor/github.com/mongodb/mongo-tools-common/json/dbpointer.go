// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

import (
	"go.mongodb.org/mongo-driver/bson/primitive"

	"fmt"
	"reflect"
)

// Transition functions for recognizing DBPointer.
// Adapted from encoding/json/scanner.go.

// stateDB is the state after reading `DB`.
func stateDBP(s *scanner, c int) int {
	if c == 'o' {
		s.step = generateState("DBPointer", []byte("inter"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal DBPointer (expecting 'o')")
}

// Decodes a DBRef literal stored in the underlying byte data into v.
func (d *decodeState) storeDBPointer(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args := d.ctorInterface()
	if len(args) != 2 {
		d.error(fmt.Errorf("expected 2 arguments to DBPointer constructor, but %v received", len(args)))
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		arg0, ok := args[0].(string)
		if !ok {
			d.error(fmt.Errorf("expected first argument to DBPointer to be of type string"))
		}
		arg1, ok := args[1].(ObjectId)
		if !ok {
			d.error(fmt.Errorf("expected second argument to DBPointer to be of type ObjectId, but ended up being %t", args[1]))
		}
		oid, err := primitive.ObjectIDFromHex(string(arg1))
		if err != nil {
			d.error(fmt.Errorf("cannot parse ObjectID from string %v: %v", arg1, err))
		}
		v.Set(reflect.ValueOf(DBPointer{arg0, oid}))
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", dbPointerType, kind))
	}
}

// Returns a DBRef literal from the underlying byte data.
func (d *decodeState) getDBPointer() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("DBPointer", 2, len(args)); err != nil {
		d.error(err)
	}
	arg0, ok := args[0].(string)
	if !ok {
		d.error(fmt.Errorf("expected string for first argument of DBPointer constructor"))
	}
	arg1, ok := args[1].(ObjectId)
	if !ok {
		d.error(fmt.Errorf("expected ObjectId for second argument of DBPointer constructor"))
	}
	oid, err := primitive.ObjectIDFromHex(string(arg1))
	if err != nil {
		d.error(fmt.Errorf("cannot parse ObjectID from string %v: %v", arg1, err))
	}

	return DBPointer{arg0, oid}
}
