package util

import (
	"gopkg.in/mgo.v2/bson"
	"reflect"
)

// IsTruthy returns true for values the server will interpret as "true".
// True values include {}, [], "", true, and any numbers != 0
func IsTruthy(val interface{}) bool {
	if val == nil {
		return false
	}
	if val == bson.Undefined {
		return false
	}

	v := reflect.ValueOf(val)
	switch v.Kind() {
	case reflect.Map, reflect.Slice, reflect.Array, reflect.String, reflect.Struct:
		return true
	default:
		z := reflect.Zero(v.Type())
		return v.Interface() != z.Interface()
	}
}

// IsFalsy returns true for values the server will interpret as "false".
// False values include numbers == 0, false, and nil
func IsFalsy(val interface{}) bool {
	return !IsTruthy(val)
}
