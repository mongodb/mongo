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

func StringSliceContains(slice []string, str string) bool {
	if slice == nil {
		return false
	}
	for _, element := range slice {
		if element == str {
			return true
		}
	}
	return false
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
