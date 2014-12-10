package mongoimport

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"os"
	"testing"
)

// TODO: currently doesn't work for lines like `a, b, "cccc,cccc", d`
func TestTSVStreamDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("With a TSV input reader", t, func() {
		Convey("integer valued strings should be converted", func() {
			contents := "1\t2\t3e\n"
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", "3e"},
			}
			tsvInputReader := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go tsvInputReader.StreamDocument(true, docChan, errChan)
			So(<-docChan, ShouldResemble, expectedRead)
			So(<-errChan, ShouldBeNil)
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
			tsvInputReader := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go tsvInputReader.StreamDocument(true, docChan, errChan)
			So(<-docChan, ShouldResemble, expectedRead)
			So(<-errChan, ShouldBeNil)
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
			tsvInputReader := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go tsvInputReader.StreamDocument(true, docChan, errChan)
			So(<-docChan, ShouldResemble, expectedRead)
			So(<-errChan, ShouldBeNil)
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
			tsvInputReader := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go tsvInputReader.StreamDocument(true, docChan, errChan)
			for i := 0; i < len(expectedReads); i++ {
				for j, readDocument := range <-docChan {
					So(readDocument.Name, ShouldEqual, expectedReads[i][j].Name)
					So(readDocument.Value, ShouldEqual, expectedReads[i][j].Value)
				}
			}
			So(<-errChan, ShouldBeNil)
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
			tsvInputReader := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go tsvInputReader.StreamDocument(true, docChan, errChan)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
			So(<-errChan, ShouldBeNil)
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
				tsvInputReader := NewTSVInputReader(fields, fileHandle, 1)
				errChan := make(chan error)
				docChan := make(chan bson.D, 1)
				go tsvInputReader.StreamDocument(true, docChan, errChan)
				So(<-docChan, ShouldResemble, expectedReadOne)
				So(<-docChan, ShouldResemble, expectedReadTwo)
				So(<-errChan, ShouldBeNil)
			})
	})
}

func TestTSVReadAndValidateHeader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("With a TSV input reader", t, func() {
		Convey("setting the header should read the first line of the TSV", func() {
			contents := "extraHeader1\textraHeader2\textraHeader3\n"
			fields := []string{}
			tsvInputReader := NewTSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(tsvInputReader.ReadAndValidateHeader(), ShouldBeNil)
			So(len(tsvInputReader.Fields), ShouldEqual, 3)
		})
	})
}

func TestTSVConvert(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("With a TSV input reader", t, func() {
		Convey("calling convert on a TSVConvertibleDoc should return the expected BSON document", func() {
			numProcessed := uint64(0)
			tsvConvertibleDoc := TSVConvertibleDoc{
				fields:       []string{"field1", "field2", "field3"},
				data:         "a\tb\tc",
				numProcessed: &numProcessed,
			}
			expectedDocument := bson.D{
				bson.DocElem{"field1", "a"},
				bson.DocElem{"field2", "b"},
				bson.DocElem{"field3", "c"},
			}
			document, err := tsvConvertibleDoc.Convert()
			So(err, ShouldBeNil)
			So(document, ShouldResemble, expectedDocument)
		})
	})
}
