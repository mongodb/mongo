// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package bsonutil

import (
	"github.com/mongodb/mongo-tools/common/json"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestRegExpValue(t *testing.T) {

	Convey("When converting JSON with RegExp values", t, func() {

		Convey("works for RegExp constructor", func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: json.RegExp{"foo", "i"},
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldResemble, bson.RegEx{"foo", "i"})
		})

		Convey(`works for RegExp document ('{ "$regex": "foo", "$options": "i" }')`, func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: map[string]interface{}{
					"$regex":   "foo",
					"$options": "i",
				},
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldResemble, bson.RegEx{"foo", "i"})
		})

		Convey(`can use multiple options ('{ "$regex": "bar", "$options": "gims" }')`, func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: map[string]interface{}{
					"$regex":   "bar",
					"$options": "gims",
				},
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldResemble, bson.RegEx{"bar", "gims"})
		})

		Convey(`fails for an invalid option ('{ "$regex": "baz", "$options": "y" }')`, func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: map[string]interface{}{
					"$regex":   "baz",
					"$options": "y",
				},
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldNotBeNil)
		})
	})
}
