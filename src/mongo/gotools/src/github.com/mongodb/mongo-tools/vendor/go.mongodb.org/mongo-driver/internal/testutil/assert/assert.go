// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package assert

import (
	"reflect"
	"sync"
	"testing"

	"github.com/google/go-cmp/cmp"
)

var cmpOpts sync.Map
var errorCompareFn = func(e1, e2 error) bool {
	if e1 == nil || e2 == nil {
		return e1 == nil && e2 == nil
	}

	return e1.Error() == e2.Error()
}
var errorCompareOpts = cmp.Options{cmp.Comparer(errorCompareFn)}

// RegisterOpts registers go-cmp options for a type. These options will be used when comparing two objects for equality.
func RegisterOpts(t reflect.Type, opts ...cmp.Option) {
	cmpOpts.Store(t, cmp.Options(opts))
}

// Equal compares first and second for equality. The objects must be of the same type.
// If the objects are not equal, the test will be failed with an error message containing msg and args.
func Equal(t testing.TB, first, second interface{}, msg string, args ...interface{}) {
	t.Helper()
	if !cmp.Equal(first, second, getCmpOpts(first)...) {
		t.Fatalf(msg, args...)
	}
}

// NotEqual compares first and second for inequality. The objects must be of the same type.
func NotEqual(t testing.TB, first, second interface{}, msg string, args ...interface{}) {
	t.Helper()
	if cmp.Equal(first, second, getCmpOpts(first)...) {
		t.Fatalf(msg, args...)
	}
}

// True asserts that the obj parameter is a boolean with value true.
func True(t testing.TB, obj interface{}, msg string, args ...interface{}) {
	t.Helper()
	b, ok := obj.(bool)
	if !ok || !b {
		t.Fatalf(msg, args...)
	}
}

// False asserts that the obj parameter is a boolean with value false.
func False(t testing.TB, obj interface{}, msg string, args ...interface{}) {
	t.Helper()
	b, ok := obj.(bool)
	if !ok || b {
		t.Fatalf(msg, args...)
	}
}

// Nil asserts that the obj parameter is nil.
func Nil(t testing.TB, obj interface{}, msg string, args ...interface{}) {
	t.Helper()
	if !isNil(obj) {
		t.Fatalf(msg, args...)
	}
}

// NotNil asserts that the obj parameter is not nil.
func NotNil(t testing.TB, obj interface{}, msg string, args ...interface{}) {
	t.Helper()
	if isNil(obj) {
		t.Fatalf(msg, args...)
	}
}

func getCmpOpts(obj interface{}) cmp.Options {
	opts, ok := cmpOpts.Load(reflect.TypeOf(obj))
	if ok {
		return opts.(cmp.Options)
	}

	if _, ok := obj.(error); ok {
		return errorCompareOpts
	}
	return nil
}

func isNil(object interface{}) bool {
	if object == nil {
		return true
	}

	val := reflect.ValueOf(object)
	switch val.Kind() {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map, reflect.Ptr, reflect.Slice:
		return val.IsNil()
	default:
		return false
	}
}
