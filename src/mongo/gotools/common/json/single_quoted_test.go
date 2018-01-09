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

func TestSingleQuotedKeys(t *testing.T) {

	Convey("When unmarshalling JSON with single quotes around its keys", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "value"
			data := fmt.Sprintf(`{'%v':"%v"}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key], ShouldEqual, value)
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "value1", "value2", "value3"
			data := fmt.Sprintf(`{'%v':"%v",'%v':"%v",'%v':"%v"}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key1], ShouldEqual, value1)
			So(jsonMap[key2], ShouldEqual, value2)
			So(jsonMap[key3], ShouldEqual, value3)
		})
	})
}

func TestSingleQuotedValues(t *testing.T) {

	Convey("When unmarshalling JSON with single quotes around its values", t, func() {

		Convey("works for a single value", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "value"
			data := fmt.Sprintf(`{"%v":'%v'}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key], ShouldEqual, value)
		})

		Convey("works for multiple values", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "value1", "value2", "value3"
			data := fmt.Sprintf(`{"%v":'%v',"%v":'%v',"%v":'%v'}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key1], ShouldEqual, value1)
			So(jsonMap[key2], ShouldEqual, value2)
			So(jsonMap[key3], ShouldEqual, value3)
		})

		Convey("can be used within BinData constructor", func() {
			var jsonMap map[string]interface{}

			key := "bindata"
			value := "BinData(1, 'xyz')"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(BinData)
			So(ok, ShouldBeTrue)
			So(jsonValue.Type, ShouldEqual, 1)
			So(jsonValue.Base64, ShouldEqual, "xyz")
		})

		Convey("can be used within Boolean constructor", func() {
			var jsonMap map[string]interface{}

			key := "boolean"
			value := "Boolean('xyz')"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)
		})

		Convey("can be used within DBRef constructor", func() {
			var jsonMap map[string]interface{}

			key := "dbref"
			value := "DBRef('examples', 'xyz')"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(DBRef)
			So(ok, ShouldBeTrue)
			So(jsonValue.Collection, ShouldEqual, "examples")
			So(jsonValue.Id, ShouldEqual, "xyz")
			So(jsonValue.Database, ShouldBeEmpty)
		})

		Convey("can be used within ObjectId constructor", func() {
			var jsonMap map[string]interface{}

			key := "_id"
			value := "ObjectId('xyz')"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(ObjectId)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, ObjectId("xyz"))
		})

		Convey("can be used within RegExp constructor", func() {
			var jsonMap map[string]interface{}

			key := "regex"
			value := "RegExp('xyz', 'i')"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue.Pattern, ShouldEqual, "xyz")
			So(jsonValue.Options, ShouldEqual, "i")
		})
	})
}
