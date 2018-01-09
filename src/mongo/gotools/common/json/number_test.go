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

func TestNumberIntValue(t *testing.T) {

	Convey("When unmarshalling JSON with NumberInt values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "NumberInt(123)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(NumberInt)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, NumberInt(123))
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "NumberInt(123)", "NumberInt(456)", "NumberInt(789)"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(NumberInt)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldEqual, NumberInt(123))

			jsonValue2, ok := jsonMap[key2].(NumberInt)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldEqual, NumberInt(456))

			jsonValue3, ok := jsonMap[key3].(NumberInt)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldEqual, NumberInt(789))
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "NumberInt(42)"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(NumberInt)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldEqual, NumberInt(42))
			}
		})

		Convey("can use string as argument", func() {
			key := "key"
			value := `NumberInt("123")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			jsonValue, err := UnmarshalBsonD([]byte(data))

			So(jsonValue[0].Value, ShouldEqual, NumberInt(123))
			So(err, ShouldBeNil)
		})

		Convey("can specify argument in hexadecimal", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "NumberInt(0x5f)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(NumberInt)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, NumberInt(0x5f))
		})
	})
}

func TestNumberLongValue(t *testing.T) {

	Convey("When unmarshalling JSON with NumberLong values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "NumberLong(123)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(NumberLong)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, NumberLong(123))
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "NumberLong(123)", "NumberLong(456)", "NumberLong(789)"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(NumberLong)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldEqual, NumberLong(123))

			jsonValue2, ok := jsonMap[key2].(NumberLong)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldEqual, NumberLong(456))

			jsonValue3, ok := jsonMap[key3].(NumberLong)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldEqual, NumberLong(789))
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "NumberLong(42)"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(NumberLong)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldEqual, NumberLong(42))
			}
		})

		Convey("can use string as argument", func() {
			key := "key"
			value := `NumberLong("123")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			jsonValue, err := UnmarshalBsonD([]byte(data))

			So(jsonValue[0].Value, ShouldEqual, NumberLong(123))
			So(err, ShouldBeNil)
		})

		Convey("can specify argument in hexadecimal", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "NumberLong(0x5f)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(NumberLong)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, NumberLong(0x5f))
		})
	})
}
