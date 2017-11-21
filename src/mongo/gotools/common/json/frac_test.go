// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestFractionalNumber(t *testing.T) {

	Convey("When unmarshalling JSON with fractional numeric values "+
		"without a leading zero", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := ".123"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(float64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldAlmostEqual, 0.123)
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := ".123", ".456", ".789"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(float64)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldAlmostEqual, 0.123)

			jsonValue2, ok := jsonMap[key2].(float64)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldAlmostEqual, 0.456)

			jsonValue3, ok := jsonMap[key3].(float64)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldAlmostEqual, 0.789)
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := ".42"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(float64)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldAlmostEqual, 0.42)
			}
		})

		Convey("can have a sign ('+' or '-')", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := ".106"
			data := fmt.Sprintf(`{"%v":+%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(float64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldAlmostEqual, 0.106)

			data = fmt.Sprintf(`{"%v":-%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(float64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldAlmostEqual, -0.106)
		})
	})
}
