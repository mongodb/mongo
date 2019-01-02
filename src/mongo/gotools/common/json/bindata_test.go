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

func TestBinDataValue(t *testing.T) {

	Convey("When unmarshalling JSON with BinData values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `BinData(1, "xyz")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(BinData)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, BinData{1, "xyz"})
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := `BinData(1, "abc")`,
				`BinData(2, "def")`, `BinData(3, "ghi")`
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(BinData)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldResemble, BinData{1, "abc"})

			jsonValue2, ok := jsonMap[key2].(BinData)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldResemble, BinData{2, "def"})

			jsonValue3, ok := jsonMap[key3].(BinData)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldResemble, BinData{3, "ghi"})
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `BinData(42, "10")`
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(BinData)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, BinData{42, "10"})
			}
		})

		Convey("can specify type argument using hexadecimal", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `BinData(0x5f, "xyz")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(BinData)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, BinData{0x5f, "xyz"})
		})
	})
}
