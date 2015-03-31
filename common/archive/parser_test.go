package archive

import (
	"bytes"
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"testing"
)

type testConsumer struct {
	oobd []string // header data
	ibd  []string // body data
	eof  bool
}

func (tc *testConsumer) HeaderBSON(b []byte) error {
	ss := strStruct{}
	err := bson.Unmarshal(b, &ss)
	tc.oobd = append(tc.oobd, ss.Str)
	return err
}

func (tc *testConsumer) BodyBSON(b []byte) error {
	ss := strStruct{}
	err := bson.Unmarshal(b, &ss)
	tc.ibd = append(tc.ibd, ss.Str)
	return err
}

func (tc *testConsumer) End() (err error) {
	if tc.eof {
		err = fmt.Errorf("double end")
	}
	tc.eof = true
	return err
}

type strStruct struct {
	Str string
}

var term = []byte{0xFF, 0xFF, 0xFF, 0xFF}
var notTerm = []byte{0xFF, 0xFF, 0xFF, 0xFE}

func TestParsing(t *testing.T) {

	Convey("with a parser with a simple parse consumer", t, func() {
		tc := &testConsumer{}
		parser := Parser{}
		Convey("A well formed header and body data", func() {
			buf := bytes.Buffer{}
			b, _ := bson.Marshal(strStruct{"header"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body"})
			buf.Write(b)
			buf.Write(term)
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldBeNil)
			So(tc.eof, ShouldBeFalse)
			So(tc.oobd[0], ShouldEqual, "header")
			So(tc.ibd[0], ShouldEqual, "body")

			err = parser.ReadBlock(tc)
			So(err, ShouldEqual, io.EOF)
			So(tc.eof, ShouldBeTrue)
		})
		Convey("An incorrect terminator", func() {
			buf := bytes.Buffer{}
			b, _ := bson.Marshal(strStruct{"header"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body"})
			buf.Write(b)
			buf.Write(notTerm)
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldNotBeNil)
		})
		Convey("An empty block", func() {
			buf := bytes.Buffer{}
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldEqual, io.EOF)
			So(tc.eof, ShouldBeTrue)
		})
		Convey("with an error comming from End", func() {
			tc.eof = true
			buf := bytes.Buffer{}
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err.Error(), ShouldContainSubstring, "double end")
		})
		Convey("with an early EOF", func() {
			buf := bytes.Buffer{}
			b, _ := bson.Marshal(strStruct{"header"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body"})
			buf.Write(b)
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldNotBeNil)
			So(tc.eof, ShouldBeFalse)
			So(tc.oobd[0], ShouldEqual, "header")
			So(tc.ibd[0], ShouldEqual, "body")
		})
		Convey("with an bson without a null terminator", func() {
			buf := bytes.Buffer{}
			b, _ := bson.Marshal(strStruct{"header"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body"})
			buf.Write(b[:len(b)-1])
			buf.Write([]byte{0x01})
			buf.Write(notTerm)
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldNotBeNil)
			So(tc.eof, ShouldBeFalse)
			So(tc.oobd[0], ShouldEqual, "header")
			So(tc.ibd, ShouldBeNil)
		})
	})
	return
}
