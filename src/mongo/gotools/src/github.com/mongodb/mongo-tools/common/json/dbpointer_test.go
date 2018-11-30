// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestDBPointerValue(t *testing.T) {

	Convey("Unmarshalling JSON with DBPointer values", t, func() {
		key := "key"
		value := `DBPointer("ref", ObjectId("552ffe9f5739878e73d116a9"))`

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(DBPointer)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, DBPointer{"ref", bson.ObjectIdHex("552ffe9f5739878e73d116a9")})
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value2 := `DBPointer("ref2", ObjectId("552ffed95739878e73d116aa"))`
			value3 := `DBPointer("ref3", ObjectId("552fff215739878e73d116ab"))`
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(DBPointer)
			So(ok, ShouldBeTrue)

			So(jsonValue1, ShouldResemble, DBPointer{"ref", bson.ObjectIdHex("552ffe9f5739878e73d116a9")})

			jsonValue2, ok := jsonMap[key2].(DBPointer)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldResemble, DBPointer{"ref2", bson.ObjectIdHex("552ffed95739878e73d116aa")})

			jsonValue3, ok := jsonMap[key3].(DBPointer)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldResemble, DBPointer{"ref3", bson.ObjectIdHex("552fff215739878e73d116ab")})
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)
			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(DBPointer)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBPointer{"ref", bson.ObjectIdHex("552ffe9f5739878e73d116a9")})
			}
		})

		Convey("will not accept an $id type that is not an ObjectId", func() {
			value := `DBPointer("ref", 4)`
			var jsonMap map[string]interface{}

			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})

	})
}
