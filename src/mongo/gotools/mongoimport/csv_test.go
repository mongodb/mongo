// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	"bytes"
	"io"
	"os"
	"strings"
	"testing"

	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

func init() {
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

func TestCSVStreamDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a CSV input reader", t, func() {
		Convey("badly encoded CSV should result in a parsing error", func() {
			contents := `1, 2, foo"bar`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldNotBeNil)
		})
		Convey("escaped quotes are parsed correctly", func() {
			contents := `1, 2, "foo""bar"`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
		})
		Convey("multiple escaped quotes separated by whitespace parsed correctly", func() {
			contents := `1, 2, "foo"" ""bar"`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", `foo" "bar`},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})
		Convey("integer valued strings should be converted", func() {
			contents := `1, 2, " 3e"`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", " 3e"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})
		Convey("extra fields should be prefixed with 'field'", func() {
			contents := `1, 2f , " 3e" , " may"`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", int32(1)},
				{"b", "2f"},
				{"c", " 3e"},
				{"field3", " may"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})
		Convey("nested CSV fields should be imported properly", func() {
			contents := `1, 2f , " 3e" , " may"`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b.c", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			b := bson.D{{"c", "2f"}}
			expectedRead := bson.D{
				{"a", int32(1)},
				{"b", b},
				{"c", " 3e"},
				{"field3", " may"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 4)
			So(r.StreamDocument(true, docChan), ShouldBeNil)

			readDocument := <-docChan
			So(readDocument[0], ShouldResemble, expectedRead[0])
			So(readDocument[1].Name, ShouldResemble, expectedRead[1].Name)
			So(*readDocument[1].Value.(*bson.D), ShouldResemble, expectedRead[1].Value)
			So(readDocument[2], ShouldResemble, expectedRead[2])
			So(readDocument[3], ShouldResemble, expectedRead[3])
		})
		Convey("whitespace separated quoted strings are still an error", func() {
			contents := `1, 2, "foo"  "bar"`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldNotBeNil)
		})
		Convey("nested CSV fields causing header collisions should error", func() {
			contents := `1, 2f , " 3e" , " may", june`
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b.c", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field3", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldNotBeNil)
		})
		Convey("calling StreamDocument() for CSVs should return next set of "+
			"values", func() {
			contents := "1, 2, 3\n4, 5, 6"
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedReadOne := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", int32(3)},
			}
			expectedReadTwo := bson.D{
				{"a", int32(4)},
				{"b", int32(5)},
				{"c", int32(6)},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 2)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
		})
		Convey("valid CSV input file that starts with the UTF-8 BOM should "+
			"not raise an error", func() {
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedReads := []bson.D{
				{
					{"a", int32(1)},
					{"b", int32(2)},
					{"c", int32(3)},
				}, {
					{"a", int32(4)},
					{"b", int32(5)},
					{"c", int32(6)},
				},
			}
			fileHandle, err := os.Open("testdata/test_bom.csv")
			So(err, ShouldBeNil)
			r := NewCSVInputReader(colSpecs, fileHandle, os.Stdout, 1, false)
			docChan := make(chan bson.D, len(expectedReads))
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			for _, expectedRead := range expectedReads {
				for i, readDocument := range <-docChan {
					So(readDocument.Name, ShouldResemble, expectedRead[i].Name)
					So(readDocument.Value, ShouldResemble, expectedRead[i].Value)
				}
			}
		})
	})
}

func TestCSVReadAndValidateHeader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	var err error
	Convey("With a CSV input reader", t, func() {
		Convey("setting the header should read the first line of the CSV", func() {
			contents := "extraHeader1, extraHeader2, extraHeader3"
			colSpecs := []ColumnSpec{}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.colSpecs), ShouldEqual, 3)
		})

		Convey("setting non-colliding nested CSV headers should not raise an error", func() {
			contents := "a, b, c"
			colSpecs := []ColumnSpec{}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.colSpecs), ShouldEqual, 3)
			contents = "a.b.c, a.b.d, c"
			colSpecs = []ColumnSpec{}
			r = NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.colSpecs), ShouldEqual, 3)

			contents = "a.b, ab, a.c"
			colSpecs = []ColumnSpec{}
			r = NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.colSpecs), ShouldEqual, 3)

			contents = "a, ab, ac, dd"
			colSpecs = []ColumnSpec{}
			r = NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.colSpecs), ShouldEqual, 4)
		})

		Convey("setting colliding nested CSV headers should raise an error", func() {
			contents := "a, a.b, c"
			colSpecs := []ColumnSpec{}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldNotBeNil)

			contents = "a.b.c, a.b.d.c, a.b.d"
			colSpecs = []ColumnSpec{}
			r = NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldNotBeNil)

			contents = "a, a, a"
			colSpecs = []ColumnSpec{}
			r = NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header that ends in a dot should error", func() {
			contents := "c, a., b"
			colSpecs := []ColumnSpec{}
			So(err, ShouldBeNil)
			So(NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false).ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header that starts in a dot should error", func() {
			contents := "c, .a, b"
			colSpecs := []ColumnSpec{}
			So(NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false).ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header that contains multiple consecutive dots should error", func() {
			contents := "c, a..a, b"
			colSpecs := []ColumnSpec{}
			So(NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false).ReadAndValidateHeader(), ShouldNotBeNil)

			contents = "c, a.a, b.b...b"
			colSpecs = []ColumnSpec{}
			So(NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false).ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header using an empty file should return EOF", func() {
			contents := ""
			colSpecs := []ColumnSpec{}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldEqual, io.EOF)
			So(len(r.colSpecs), ShouldEqual, 0)
		})
		Convey("setting the header with column specs already set should replace "+
			"the existing column specs", func() {
			contents := "extraHeader1,extraHeader2,extraHeader3"
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			r := NewCSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			// if ReadAndValidateHeader() is called with column specs already passed
			// in, the header should be replaced with the read header line
			So(len(r.colSpecs), ShouldEqual, 3)
			So(ColumnNames(r.colSpecs), ShouldResemble, strings.Split(contents, ","))
		})
		Convey("plain CSV input file sources should be parsed correctly and "+
			"subsequent imports should parse correctly", func() {
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedReadOne := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", int32(3)},
			}
			expectedReadTwo := bson.D{
				{"a", int32(3)},
				{"b", 5.4},
				{"c", "string"},
			}
			fileHandle, err := os.Open("testdata/test.csv")
			So(err, ShouldBeNil)
			r := NewCSVInputReader(colSpecs, fileHandle, os.Stdout, 1, false)
			docChan := make(chan bson.D, 50)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
		})
	})
}

func TestCSVConvert(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a CSV input reader", t, func() {
		Convey("calling convert on a CSVConverter should return the expected BSON document", func() {
			csvConverter := CSVConverter{
				colSpecs: []ColumnSpec{
					{"field1", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field2", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field3", new(FieldAutoParser), pgAutoCast, "auto"},
				},
				data:  []string{"a", "b", "c"},
				index: uint64(0),
			}
			expectedDocument := bson.D{
				{"field1", "a"},
				{"field2", "b"},
				{"field3", "c"},
			}
			document, err := csvConverter.Convert()
			So(err, ShouldBeNil)
			So(document, ShouldResemble, expectedDocument)
		})
	})
}
