package mongoimport

import (
	"bytes"
	"os"
	"testing"

	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

func TestTSVStreamDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a TSV input reader", t, func() {
		Convey("integer valued strings should be converted tsv1", func() {
			contents := "1\t2\t3e\n"
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", "3e"},
			}
			r := NewTSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("valid TSV input file that starts with the UTF-8 BOM should "+
			"not raise an error", func() {
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", int32(3)},
			}
			fileHandle, err := os.Open("testdata/test_bom.tsv")
			So(err, ShouldBeNil)
			r := NewTSVInputReader(colSpecs, fileHandle, os.Stdout, 1, false)
			docChan := make(chan bson.D, 2)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("integer valued strings should be converted tsv2", func() {
			contents := "a\tb\t\"cccc,cccc\"\td\n"
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", "a"},
				{"b", "b"},
				{"c", `"cccc,cccc"`},
				{"field3", "d"},
			}
			r := NewTSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("extra columns should be prefixed with 'field'", func() {
			contents := "1\t2\t3e\t may\n"
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", "3e"},
				{"field3", " may"},
			}
			r := NewTSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("mixed values should be parsed correctly", func() {
			contents := "12\t13.3\tInline\t14\n"
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
				{"d", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			expectedRead := bson.D{
				{"a", int32(12)},
				{"b", 13.3},
				{"c", "Inline"},
				{"d", int32(14)},
			}
			r := NewTSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("calling StreamDocument() in succession for TSVs should "+
			"return the correct next set of values", func() {
			contents := "1\t2\t3\n4\t5\t6\n"
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
			r := NewTSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, len(expectedReads))
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			for i := 0; i < len(expectedReads); i++ {
				for j, readDocument := range <-docChan {
					So(readDocument.Name, ShouldEqual, expectedReads[i][j].Name)
					So(readDocument.Value, ShouldEqual, expectedReads[i][j].Value)
				}
			}
		})

		Convey("calling StreamDocument() in succession for TSVs that contain "+
			"quotes should return the correct next set of values", func() {
			contents := "1\t2\t3\n4\t\"\t6\n"
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
				{"b", `"`},
				{"c", int32(6)},
			}
			r := NewTSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			docChan := make(chan bson.D, 2)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
		})

		Convey("plain TSV input file sources should be parsed correctly and "+
			"subsequent imports should parse correctly",
			func() {
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
					{"b", 4.6},
					{"c", int32(5)},
				}
				fileHandle, err := os.Open("testdata/test.tsv")
				So(err, ShouldBeNil)
				r := NewTSVInputReader(colSpecs, fileHandle, os.Stdout, 1, false)
				docChan := make(chan bson.D, 50)
				So(r.StreamDocument(true, docChan), ShouldBeNil)
				So(<-docChan, ShouldResemble, expectedReadOne)
				So(<-docChan, ShouldResemble, expectedReadTwo)
			})
	})
}

func TestTSVReadAndValidateHeader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a TSV input reader", t, func() {
		Convey("setting the header should read the first line of the TSV", func() {
			contents := "extraHeader1\textraHeader2\textraHeader3\n"
			colSpecs := []ColumnSpec{}
			r := NewTSVInputReader(colSpecs, bytes.NewReader([]byte(contents)), os.Stdout, 1, false)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.colSpecs), ShouldEqual, 3)
		})
	})
}

func TestTSVConvert(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a TSV input reader", t, func() {
		Convey("calling convert on a TSVConverter should return the expected BSON document", func() {
			tsvConverter := TSVConverter{
				colSpecs: []ColumnSpec{
					{"field1", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field2", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field3", new(FieldAutoParser), pgAutoCast, "auto"},
				},
				data:  "a\tb\tc",
				index: uint64(0),
			}
			expectedDocument := bson.D{
				{"field1", "a"},
				{"field2", "b"},
				{"field3", "c"},
			}
			document, err := tsvConverter.Convert()
			So(err, ShouldBeNil)
			So(document, ShouldResemble, expectedDocument)
		})
	})
}
