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

// Transition functions for recognizing ISODate.
// Adapted from encoding/json/scanner.go.

// stateIS is the state after reading `IS`.
func stateIS(s *scanner, c int) int {
    if c == 'O' {
        s.step = stateISO
        return scanContinue
    }
    return s.error(c, "in literal ISODate (expecting 'O')")
}

// stateISO is the state after reading `ISO`.
func stateISO(s *scanner, c int) int {
    if c == 'D' {
        s.step = stateD
        return scanContinue
    }
    return s.error(c, "in literal ISODate (expecting 'D')")
}

// Decodes a ISODate literal stored in the underlying byte data into v.
func (d *decodeState) storeISODate(v reflect.Value) {
    op := d.scanWhile(scanSkipSpace)
    if op != scanBeginCtor {
        d.error(fmt.Errorf("expected beginning of constructor"))
    }
    args, err := d.ctor("ISODate", []reflect.Type{isoDateType})
    if err != nil {
        d.error(err)
    }
    switch kind := v.Kind(); kind {
    case reflect.Interface:
        v.Set(args[0])
    default:
        d.error(fmt.Errorf("cannot store %v value into %v type", isoDateType, kind))
    }
}
