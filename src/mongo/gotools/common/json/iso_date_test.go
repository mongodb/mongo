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

func TestISODateValue(t *testing.T) {

    Convey("When unmarshalling JSON with ISODate values", t, func() {

        Convey("works for a single key", func() {
            var jsonMap map[string]interface{}

            key := "key"
            value := "ISODate(\"2006-01-02T15:04-0700\")"
            data := fmt.Sprintf(`{"%v":%v}`, key, value)

            err := Unmarshal([]byte(data), &jsonMap)
            So(err, ShouldBeNil)

            jsonValue, ok := jsonMap[key].(ISODate)
            So(ok, ShouldBeTrue)
            So(jsonValue, ShouldEqual, ISODate("2006-01-02T15:04-0700"))
        })

        Convey("works for multiple keys", func() {
            var jsonMap map[string]interface{}

            key1, key2, key3 := "key1", "key2", "key3"
            value1, value2, value3 := "ISODate(\"2006-01-02T15:04Z0700\")", "ISODate(\"2013-01-02T15:04Z0700\")", "ISODate(\"2014-02-02T15:04Z0700\")"
            data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
                key1, value1, key2, value2, key3, value3)

            err := Unmarshal([]byte(data), &jsonMap)
            So(err, ShouldBeNil)

            jsonValue1, ok := jsonMap[key1].(ISODate)
            So(ok, ShouldBeTrue)
            So(jsonValue1, ShouldEqual, ISODate("2006-01-02T15:04Z0700"))

            jsonValue2, ok := jsonMap[key2].(ISODate)
            So(ok, ShouldBeTrue)
            So(jsonValue2, ShouldEqual, ISODate("2013-01-02T15:04Z0700"))

            jsonValue3, ok := jsonMap[key3].(ISODate)
            So(ok, ShouldBeTrue)
            So(jsonValue3, ShouldEqual, ISODate("2014-02-02T15:04Z0700"))
        })

        Convey("works in an array", func() {
            var jsonMap map[string]interface{}

            key := "key"
            value := "ISODate(\"2006-01-02T15:04-0700\")"
            data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
                key, value, value, value)

            err := Unmarshal([]byte(data), &jsonMap)
            So(err, ShouldBeNil)

            jsonArray, ok := jsonMap[key].([]interface{})
            So(ok, ShouldBeTrue)

            for _, _jsonValue := range jsonArray {
                jsonValue, ok := _jsonValue.(ISODate)
                So(ok, ShouldBeTrue)
                So(jsonValue, ShouldEqual, ISODate("2006-01-02T15:04-0700"))
            }
        })

        Convey("will take valid format 2006-01-02T15:04:05.000-0700", func() {
            var jsonMap map[string]interface{}

            key := "key"
            value := "ISODate(\"2006-01-02T15:04:05.000-0700\")"
            data := fmt.Sprintf(`{"%v":%v}`, key, value)

            err := Unmarshal([]byte(data), &jsonMap)
            So(err, ShouldBeNil)

            jsonValue, ok := jsonMap[key].(ISODate)
            So(ok, ShouldBeTrue)
            So(jsonValue, ShouldEqual, ISODate("2006-01-02T15:04:05.000-0700"))
        })


        Convey("will take valid format 2006-01-02T15:04:05", func() {
            var jsonMap map[string]interface{}

            key := "key"
            value := "ISODate(\"2014-01-02T15:04:05Z\")"
            data := fmt.Sprintf(`{"%v":%v}`, key, value)

            err := Unmarshal([]byte(data), &jsonMap)
            So(err, ShouldBeNil)

            jsonValue, ok := jsonMap[key].(ISODate)
            So(ok, ShouldBeTrue)
            So(jsonValue, ShouldEqual, ISODate("2014-01-02T15:04:05Z"))
        })


        Convey("will take valid format 2006-01-02T15:04-0700", func() {
            var jsonMap map[string]interface{}

            key := "key"
            value := "ISODate(\"2006-01-02T15:04-0700\")"
            data := fmt.Sprintf(`{"%v":%v}`, key, value)

            err := Unmarshal([]byte(data), &jsonMap)
            So(err, ShouldBeNil)

            jsonValue, ok := jsonMap[key].(ISODate)
            So(ok, ShouldBeTrue)
            So(jsonValue, ShouldEqual, ISODate("2006-01-02T15:04-0700"))
        })



    })
}

