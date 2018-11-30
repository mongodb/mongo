// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package bsonutil

import (
	"encoding/json"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"strings"
	"testing"
)

func TestMarshalDMarshalJSON(t *testing.T) {

	Convey("With a valid bson.D", t, func() {
		testD := bson.D{
			{"cool", "rad"},
			{"aaa", 543.2},
			{"I", 0},
			{"E", 0},
			{"map", bson.M{"1": 1, "2": "two"}},
		}

		Convey("wrapping with MarshalD should allow json.Marshal to work", func() {
			asJSON, err := json.Marshal(MarshalD(testD))
			So(err, ShouldBeNil)
			strJSON := string(asJSON)

			Convey("with order preserved", func() {
				So(strings.Index(strJSON, "cool"), ShouldBeLessThan, strings.Index(strJSON, "aaa"))
				So(strings.Index(strJSON, "aaa"), ShouldBeLessThan, strings.Index(strJSON, "I"))
				So(strings.Index(strJSON, "I"), ShouldBeLessThan, strings.Index(strJSON, "E"))
				So(strings.Index(strJSON, "E"), ShouldBeLessThan, strings.Index(strJSON, "map"))
				So(strings.Count(strJSON, ","), ShouldEqual, 5) // 4 + 1 from internal map
			})

			Convey("but still usable by the json parser", func() {
				var asMap bson.M
				err := json.Unmarshal(asJSON, &asMap)
				So(err, ShouldBeNil)

				Convey("with types & values preserved", func() {
					So(asMap["cool"], ShouldEqual, "rad")
					So(asMap["aaa"], ShouldEqual, 543.2)
					So(asMap["I"], ShouldEqual, 0)
					So(asMap["E"], ShouldEqual, 0)
					So(asMap["map"].(map[string]interface{})["1"], ShouldEqual, 1)
					So(asMap["map"].(map[string]interface{})["2"], ShouldEqual, "two")
				})
			})

			Convey("putting it inside another map should still be usable by json.Marshal", func() {
				_, err := json.Marshal(bson.M{"x": 0, "y": MarshalD(testD)})
				So(err, ShouldBeNil)
			})
		})
	})

	Convey("With en empty bson.D", t, func() {
		testD := bson.D{}

		Convey("wrapping with MarshalD should allow json.Marshal to work", func() {
			asJSON, err := json.Marshal(MarshalD(testD))
			So(err, ShouldBeNil)
			strJSON := string(asJSON)
			So(strJSON, ShouldEqual, "{}")

			Convey("but still usable by the json parser", func() {
				var asInterface interface{}
				err := json.Unmarshal(asJSON, &asInterface)
				So(err, ShouldBeNil)
				asMap, ok := asInterface.(map[string]interface{})
				So(ok, ShouldBeTrue)
				So(len(asMap), ShouldEqual, 0)
			})
		})
	})
}

func TestFindValueByKey(t *testing.T) {
	Convey("Given a bson.D document and a specific key", t, func() {
		subDocument := &bson.D{
			bson.DocElem{Name: "field4", Value: "c"},
		}
		document := &bson.D{
			bson.DocElem{Name: "field1", Value: "a"},
			bson.DocElem{Name: "field2", Value: "b"},
			bson.DocElem{Name: "field3", Value: subDocument},
		}
		Convey("the corresponding value top-level keys should be returned", func() {
			value, err := FindValueByKey("field1", document)
			So(value, ShouldEqual, "a")
			So(err, ShouldBeNil)
		})
		Convey("the corresponding value top-level keys with sub-document values should be returned", func() {
			value, err := FindValueByKey("field3", document)
			So(value, ShouldEqual, subDocument)
			So(err, ShouldBeNil)
		})
		Convey("for non-existent keys nil and an error should be returned", func() {
			value, err := FindValueByKey("field4", document)
			So(value, ShouldBeNil)
			So(err, ShouldNotBeNil)
		})
	})
}

func TestEscapedKey(t *testing.T) {
	Convey("Given a bson.D document with a key that requires escaping", t, func() {
		document := bson.D{
			bson.DocElem{Name: `foo"bar`, Value: "a"},
		}
		Convey("it can be marshaled without error", func() {
			asJSON, err := json.Marshal(MarshalD(document))
			So(err, ShouldBeNil)
			Convey("and subsequently unmarshaled without error", func() {
				var asMap bson.M
				err := json.Unmarshal(asJSON, &asMap)
				So(err, ShouldBeNil)
				Convey("with the original value being correctly found with the unescaped key", func() {
					So(asMap[`foo"bar`], ShouldEqual, "a")
				})
			})
		})
	})
}
