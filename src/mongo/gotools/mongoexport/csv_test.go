// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoexport

import (
	"bytes"
	"encoding/csv"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"strings"
	"testing"
)

func TestWriteCSV(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a CSV export output", t, func() {
		fields := []string{"_id", "x", " y", "z.1.a"}
		out := &bytes.Buffer{}

		Convey("Headers should be written correctly", func() {
			csvExporter := NewCSVExportOutput(fields, false, out)
			err := csvExporter.WriteHeader()
			So(err, ShouldBeNil)
			csvExporter.ExportDocument(bson.D{{"_id", "12345"}})
			csvExporter.WriteFooter()
			csvExporter.Flush()
			rec, err := csv.NewReader(strings.NewReader(out.String())).Read()
			So(err, ShouldBeNil)
			So(rec, ShouldResemble, []string{"_id", "x", " y", "z.1.a"})
		})

		Convey("Headers should not be written", func() {
			csvExporter := NewCSVExportOutput(fields, true, out)
			err := csvExporter.WriteHeader()
			So(err, ShouldBeNil)
			csvExporter.ExportDocument(bson.D{{"_id", "12345"}})
			csvExporter.WriteFooter()
			csvExporter.Flush()
			rec, err := csv.NewReader(strings.NewReader(out.String())).Read()
			So(err, ShouldBeNil)
			So(rec, ShouldResemble, []string{"12345", "", "", ""})
		})

		Convey("Exported document with missing fields should print as blank", func() {
			csvExporter := NewCSVExportOutput(fields, true, out)
			csvExporter.ExportDocument(bson.D{{"_id", "12345"}})
			csvExporter.WriteFooter()
			csvExporter.Flush()
			rec, err := csv.NewReader(strings.NewReader(out.String())).Read()
			So(err, ShouldBeNil)
			So(rec, ShouldResemble, []string{"12345", "", "", ""})
		})

		Convey("Exported document with index into nested objects should print correctly", func() {
			csvExporter := NewCSVExportOutput(fields, true, out)
			z := []interface{}{"x", bson.D{{"a", "T"}, {"B", 1}}}
			csvExporter.ExportDocument(bson.D{{Name: "z", Value: z}})
			csvExporter.WriteFooter()
			csvExporter.Flush()
			rec, err := csv.NewReader(strings.NewReader(out.String())).Read()
			So(err, ShouldBeNil)
			So(rec, ShouldResemble, []string{"", "", "", "T"})
		})

		Reset(func() {
			out.Reset()
		})

	})
}

func TestExtractDField(t *testing.T) {
	Convey("With a test bson.D", t, func() {
		b := []interface{}{"inner", bsonutil.MarshalD{{"inner2", 1}}}
		c := bsonutil.MarshalD{{"x", 5}}
		d := bsonutil.MarshalD{{"z", nil}}
		testD := bsonutil.MarshalD{
			{"a", "string"},
			{"b", b},
			{"c", c},
			{"d", d},
		}

		Convey("regular fields should be extracted by name", func() {
			val := extractFieldByName("a", testD)
			So(val, ShouldEqual, "string")
		})

		Convey("array fields should be extracted by name", func() {
			val := extractFieldByName("b.1", testD)
			So(val, ShouldResemble, bsonutil.MarshalD{{"inner2", 1}})
			val = extractFieldByName("b.1.inner2", testD)
			So(val, ShouldEqual, 1)
			val = extractFieldByName("b.0", testD)
			So(val, ShouldEqual, "inner")
		})

		Convey("subdocument fields should be extracted by name", func() {
			val := extractFieldByName("c", testD)
			So(val, ShouldResemble, bsonutil.MarshalD{{"x", 5}})
			val = extractFieldByName("c.x", testD)
			So(val, ShouldEqual, 5)

			Convey("even if they contain null values", func() {
				val := extractFieldByName("d", testD)
				So(val, ShouldResemble, bsonutil.MarshalD{{"z", nil}})
				val = extractFieldByName("d.z", testD)
				So(val, ShouldEqual, nil)
				val = extractFieldByName("d.z.nope", testD)
				So(val, ShouldEqual, "")
			})
		})

		Convey(`non-existing fields should return ""`, func() {
			val := extractFieldByName("f", testD)
			So(val, ShouldEqual, "")
			val = extractFieldByName("c.nope", testD)
			So(val, ShouldEqual, "")
			val = extractFieldByName("c.nope.NOPE", testD)
			So(val, ShouldEqual, "")
			val = extractFieldByName("b.1000", testD)
			So(val, ShouldEqual, "")
			val = extractFieldByName("b.1.nada", testD)
			So(val, ShouldEqual, "")
		})

	})

	Convey(`Extraction of a non-document should return ""`, t, func() {
		val := extractFieldByName("meh", []interface{}{"meh"})
		So(val, ShouldEqual, "")
	})

	Convey(`Extraction of a nil document should return ""`, t, func() {
		val := extractFieldByName("a", nil)
		So(val, ShouldEqual, "")
	})
}
