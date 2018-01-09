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

func TestObjectIdValue(t *testing.T) {

	Convey("When converting JSON with ObjectId values", t, func() {

		Convey("works for ObjectId constructor", func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: json.ObjectId("0123456789abcdef01234567"),
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldEqual, bson.ObjectIdHex("0123456789abcdef01234567"))
		})

		Convey(`works for ObjectId document ('{ "$oid": "0123456789abcdef01234567" }')`, func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: map[string]interface{}{
					"$oid": "0123456789abcdef01234567",
				},
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldEqual, bson.ObjectIdHex("0123456789abcdef01234567"))
		})
	})
}
