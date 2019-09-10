// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoexport

import (
	"bytes"
	"testing"

	"github.com/mongodb/mongo-tools-common/json"
	"github.com/mongodb/mongo-tools-common/testtype"
	. "github.com/smartystreets/goconvey/convey"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
)

func TestWriteJSON(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.UnitTestType)

	Convey("With a JSON export output", t, func() {
		out := &bytes.Buffer{}

		Convey("Special types should serialize as extended JSON", func() {

			Convey("ObjectId should have an extended JSON format", func() {
				jsonExporter := NewJSONExportOutput(false, false, out, Relaxed)
				objId := primitive.NewObjectID()
				err := jsonExporter.WriteHeader()
				So(err, ShouldBeNil)
				err = jsonExporter.ExportDocument(bson.D{{"_id", objId}})
				So(err, ShouldBeNil)
				err = jsonExporter.WriteFooter()
				So(err, ShouldBeNil)
				So(out.String(), ShouldEqual, `{"_id":{"$oid":"`+objId.Hex()+`"}}`+"\n")
			})

			Convey("Canoncial format should be outputted if canonical is specified", func() {
				exporter := NewJSONExportOutput(false, false, out, Canonical)

				err := exporter.WriteHeader()
				So(err, ShouldBeNil)

				err = exporter.ExportDocument(bson.D{{"x", int32(1)}})
				So(err, ShouldBeNil)

				err = exporter.WriteFooter()
				So(err, ShouldBeNil)

				So(out.String(), ShouldEqual, `{"x":{"$numberInt":"1"}}`+"\n")
			})

			Reset(func() {
				out.Reset()
			})
		})

	})
}

func TestJSONArray(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.UnitTestType)

	Convey("With a JSON export output in array mode", t, func() {
		out := &bytes.Buffer{}
		Convey("exporting a bunch of documents should produce valid json", func() {
			jsonExporter := NewJSONExportOutput(true, false, out, Relaxed)
			err := jsonExporter.WriteHeader()
			So(err, ShouldBeNil)

			// Export a few docs of various types

			testObjs := []interface{}{primitive.NewObjectID(), "asd", 12345, 3.14159, bson.D{{"A", 1}}}
			for _, obj := range testObjs {
				err = jsonExporter.ExportDocument(bson.D{{"_id", obj}})
				So(err, ShouldBeNil)
			}

			err = jsonExporter.WriteFooter()
			So(err, ShouldBeNil)
			// Unmarshal the whole thing, it should be valid json
			fromJSON := []map[string]interface{}{}
			err = json.Unmarshal(out.Bytes(), &fromJSON)
			So(err, ShouldBeNil)
			So(len(fromJSON), ShouldEqual, len(testObjs))

		})

		Reset(func() {
			out.Reset()
		})

	})
}
