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

func TestRegExpValue(t *testing.T) {

	Convey("When unmarshalling JSON with RegExp values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `RegExp("foo", "i")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, RegExp{"foo", "i"})
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := `RegExp("foo", "i")`,
				`RegExp("bar", "i")`, `RegExp("baz", "i")`
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldResemble, RegExp{"foo", "i"})

			jsonValue2, ok := jsonMap[key2].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldResemble, RegExp{"bar", "i"})

			jsonValue3, ok := jsonMap[key3].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldResemble, RegExp{"baz", "i"})
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `RegExp("xyz", "i")`
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(RegExp)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, RegExp{"xyz", "i"})
			}
		})

		Convey("can use options 'g', 'i', 'm', and 's'", func() {
			var jsonMap map[string]interface{}

			key := "key"
			options := []string{"g", "i", "m", "s"}

			for _, option := range options {
				data := fmt.Sprintf(`{"%v":RegExp("xyz", "%v")}`, key, option)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(RegExp)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, RegExp{"xyz", option})
			}
		})

		Convey("can use multiple options", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `RegExp("foo", "gims")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, RegExp{"foo", "gims"})
		})
	})
}

func TestRegexpLiteral(t *testing.T) {

	Convey("When unmarshalling JSON with regular expression literals", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "/foo/i"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, RegExp{"foo", "i"})
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "/foo/i", "/bar/i", "/baz/i"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldResemble, RegExp{"foo", "i"})

			jsonValue2, ok := jsonMap[key2].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldResemble, RegExp{"bar", "i"})

			jsonValue3, ok := jsonMap[key3].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldResemble, RegExp{"baz", "i"})
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "/xyz/i"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(RegExp)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, RegExp{"xyz", "i"})
			}
		})

		Convey("can use options 'g', 'i', 'm', and 's'", func() {
			var jsonMap map[string]interface{}

			key := "key"
			options := []string{"g", "i", "m", "s"}

			for _, option := range options {
				data := fmt.Sprintf(`{"%v":/xyz/%v}`, key, option)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(RegExp)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, RegExp{"xyz", option})
			}
		})

		Convey("can use multiple options", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "/foo/gims"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, RegExp{"foo", "gims"})
		})

		Convey("can contain unescaped quotes (`'` and `\"`)", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `/f'o"o/i`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(RegExp)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, RegExp{`f'o"o`, "i"})
		})

		Convey("cannot contain unescaped forward slashes ('/')", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "/f/o/o/i"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})

		Convey("cannot contain invalid escape sequences", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `/f\o\o/`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})
	})
}
