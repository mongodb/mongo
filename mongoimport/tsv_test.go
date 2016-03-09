package mongoimport

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"os"
	"testing"
)

func TestTSVStreamDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a TSV input reader", t, func() {
		Convey("integer valued strings should be converted tsv1", func() {
			contents := "1\t2\t3e\n"
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", "3e"},
			}
			r := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("integer valued strings should be converted tsv2", func() {
			contents := "a\tb\t\"cccc,cccc\"\td\n"
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", "a"},
				bson.DocElem{"b", "b"},
				bson.DocElem{"c", `"cccc,cccc"`},
				bson.DocElem{"field3", "d"},
			}
			r := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("extra fields should be prefixed with 'field'", func() {
			contents := "1\t2\t3e\t may\n"
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", "3e"},
				bson.DocElem{"field3", " may"},
			}
			r := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("mixed values should be parsed correctly", func() {
			contents := "12\t13.3\tInline\t14\n"
			fields := []string{"a", "b", "c", "d"}
			expectedRead := bson.D{
				bson.DocElem{"a", 12},
				bson.DocElem{"b", 13.3},
				bson.DocElem{"c", "Inline"},
				bson.DocElem{"d", 14},
			}
			r := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})

		Convey("calling StreamDocument() in succession for TSVs should "+
			"return the correct next set of values", func() {
			contents := "1\t2\t3\n4\t5\t6\n"
			fields := []string{"a", "b", "c"}
			expectedReads := []bson.D{
				bson.D{
					bson.DocElem{"a", 1},
					bson.DocElem{"b", 2},
					bson.DocElem{"c", 3},
				},
				bson.D{
					bson.DocElem{"a", 4},
					bson.DocElem{"b", 5},
					bson.DocElem{"c", 6},
				},
			}
			r := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
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
			fields := []string{"a", "b", "c"}
			expectedReadOne := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", 3},
			}
			expectedReadTwo := bson.D{
				bson.DocElem{"a", 4},
				bson.DocElem{"b", `"`},
				bson.DocElem{"c", 6},
			}
			r := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 2)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
		})

		Convey("plain TSV input file sources should be parsed correctly and "+
			"subsequent imports should parse correctly",
			func() {
				fields := []string{"a", "b", "c"}
				expectedReadOne := bson.D{
					bson.DocElem{"a", 1},
					bson.DocElem{"b", 2},
					bson.DocElem{"c", 3},
				}
				expectedReadTwo := bson.D{
					bson.DocElem{"a", 3},
					bson.DocElem{"b", 4.6},
					bson.DocElem{"c", 5},
				}
				fileHandle, err := os.Open("testdata/test.tsv")
				So(err, ShouldBeNil)
				r := NewTSVInputReader(fields, fileHandle, 1)
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
			fields := []string{}
			r := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.fields), ShouldEqual, 3)
		})
	})
}

func TestTSVConvert(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a TSV input reader", t, func() {
		Convey("calling convert on a TSVConverter should return the expected BSON document", func() {
			tsvConverter := TSVConverter{
				fields: []string{"field1", "field2", "field3"},
				data:   "a\tb\tc",
				index:  uint64(0),
			}
			expectedDocument := bson.D{
				bson.DocElem{"field1", "a"},
				bson.DocElem{"field2", "b"},
				bson.DocElem{"field3", "c"},
			}
			document, err := tsvConverter.Convert()
			So(err, ShouldBeNil)
			So(document, ShouldResemble, expectedDocument)
		})
	})
}
