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

func TestTimestampValue(t *testing.T) {

	Convey("When unmarshalling JSON with Timestamp values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Timestamp(123, 321)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(Timestamp)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, Timestamp{123, 321})
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "Timestamp(123, 321)",
				"Timestamp(456, 654)", "Timestamp(789, 987)"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(Timestamp)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldResemble, Timestamp{123, 321})

			jsonValue2, ok := jsonMap[key2].(Timestamp)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldResemble, Timestamp{456, 654})

			jsonValue3, ok := jsonMap[key3].(Timestamp)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldResemble, Timestamp{789, 987})
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Timestamp(42, 10)"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(Timestamp)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, Timestamp{42, 10})
			}
		})

		Convey("cannot use string as argument", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `Timestamp("123", "321")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})
	})
}
