package db

import (
	"bytes"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io/ioutil"
	"testing"
)

func TestBufferlessBSONSource(t *testing.T) {
	var testValues = []bson.M{
		{"_": bson.Binary{Kind: 0x80, Data: []byte("apples")}},
		{"_": bson.Binary{Kind: 0x80, Data: []byte("bananas")}},
		{"_": bson.Binary{Kind: 0x80, Data: []byte("cherries")}},
	}
	Convey("with a buffer containing several bson documents with binary fields", t, func() {
		writeBuf := bytes.NewBuffer(make([]byte, 0, 1024))
		for _, tv := range testValues {
			data, err := bson.Marshal(&tv)
			So(err, ShouldBeNil)
			_, err = writeBuf.Write(data)
			So(err, ShouldBeNil)
		}
		Convey("that we parse correctly with a BufferlessBSONSource", func() {
			bsonSource := NewDecodedBSONSource(
				NewBufferlessBSONSource(ioutil.NopCloser(writeBuf)))
			docs := []bson.M{}
			count := 0
			doc := &bson.M{}
			for bsonSource.Next(doc) {
				count++
				docs = append(docs, *doc)
				doc = &bson.M{}
			}
			So(bsonSource.Err(), ShouldBeNil)
			So(count, ShouldEqual, len(testValues))
			So(docs, ShouldResemble, testValues)
		})
	})
}
