package db

import (
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestShimRead(t *testing.T) {
	Convey("Test shim process", t, func() {
		var bsonTool StorageShim
		resetFunc := func() {
			//invokes a fake shim that just cat's a bson file
			bsonTool = StorageShim{
				DBPath:     "/data/db",
				Database:   "mci_dev",
				Collection: "tasks",
				Query:      "{}",
				Skip:       0,
				Limit:      0,
				ShimPath:   "testdata/mock_shim.sh",
			}
		}
		Reset(resetFunc)
		resetFunc()

		Convey("with raw byte stream", func() {
			iter, err := bsonTool.Open()
			if err != nil {
				t.Fatal(err)
			}

			docCount := 0
			docBytes := make([]byte, MaxBSONSize)
			for {
				if hasDoc, docSize := iter.LoadNextInto(docBytes); hasDoc {
					val := map[string]interface{}{}
					bson.Unmarshal(docBytes[0:docSize], val)
					docCount++
				} else {
					break
				}
			}
			So(iter.Err(), ShouldBeNil)
			So(docCount, ShouldEqual, 100)
		})

		Convey("with decoded byte stream", func() {
			iter, err := bsonTool.Open()
			if err != nil {
				t.Fatal(err)
			}

			decStrm := NewDecodedBSONStream(iter)
			docVal := bson.M{}
			docCount := 0
			for decStrm.Next(docVal) {
				docCount++
			}
			So(iter.Err(), ShouldBeNil)
			So(docCount, ShouldEqual, 100)
		})
	})
}
