// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	"fmt"
	"reflect"
)

// generic function that returns true if elt is in slice.
// panics if slice is not of Kind reflect.Slice
func SliceContains(slice, elt interface{}) bool {
	if slice == nil {
		return false
	}
	v := reflect.ValueOf(slice)
	if v.Kind() != reflect.Slice {
		panic(fmt.Sprintf("Cannot call SliceContains on a non-slice %#v of "+
			"kind %#v", slice, v.Kind().String()))
	}
	for i := 0; i < v.Len(); i++ {
		if reflect.DeepEqual(v.Index(i).Interface(), elt) {
			return true
		}
	}
	return false
}

// StringSliceContains reports whether str is in the slice.
func StringSliceContains(slice []string, str string) bool {
	return StringSliceIndex(slice, str) != -1
}

// StringSliceContains returns the first index at which the given element
// can be found in the slice, or -1 if it is not present.
func StringSliceIndex(slice []string, str string) int {
	i := -1
	for j, v := range slice {
		if v == str {
			i = j
			break
		}
	}
	return i
}

// generic function that returns number of instances of 'elt' in 'slice'.
// panics if slice is not of Kind reflect.Slice
func SliceCount(slice, elt interface{}) int {
	v := reflect.ValueOf(slice)
	if v.Kind() != reflect.Slice {
		panic(fmt.Sprintf("Cannot call SliceCount on a non-slice %#v of kind "+
			"%#v", slice, v.Kind().String()))
	}
	counter := 0
	for i := 0; i < v.Len(); i++ {
		if reflect.DeepEqual(v.Index(i).Interface(), elt) {
			counter++
		}
	}
	return counter
}
