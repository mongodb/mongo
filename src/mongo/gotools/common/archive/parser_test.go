// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package archive

import (
	"bytes"
	"fmt"
	"io"
	"testing"

	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

type testConsumer struct {
	headers []string // header data
	bodies  []string // body data
	eof     bool
}

func (tc *testConsumer) HeaderBSON(b []byte) error {
	ss := strStruct{}
	err := bson.Unmarshal(b, &ss)
	tc.headers = append(tc.headers, ss.Str)
	return err
}

func (tc *testConsumer) BodyBSON(b []byte) error {
	ss := strStruct{}
	err := bson.Unmarshal(b, &ss)
	tc.bodies = append(tc.bodies, ss.Str)
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

	Convey("With a parser with a simple parser consumer", t, func() {
		tc := &testConsumer{}
		parser := Parser{}
		Convey("a well formed header and body", func() {
			buf := bytes.Buffer{}
			b, _ := bson.Marshal(strStruct{"header"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body"})
			buf.Write(b)
			buf.Write(term)
			parser.In = &buf
			Convey("ReadBlock data parses correctly", func() {
				err := parser.ReadBlock(tc)
				So(err, ShouldBeNil)
				So(tc.eof, ShouldBeFalse)
				So(tc.headers[0], ShouldEqual, "header")
				So(tc.bodies[0], ShouldEqual, "body")

				err = parser.ReadBlock(tc)
				So(err, ShouldEqual, io.EOF)
			})
			Convey("ReadAllBlock data parses correctly", func() {
				err := parser.ReadAllBlocks(tc)
				So(err, ShouldEqual, nil)
				So(tc.eof, ShouldBeTrue)
				So(tc.headers[0], ShouldEqual, "header")
				So(tc.bodies[0], ShouldEqual, "body")

			})
		})
		Convey("a well formed header and multiple body datas parse correctly", func() {
			buf := bytes.Buffer{}
			b, _ := bson.Marshal(strStruct{"header"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body0"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body1"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body2"})
			buf.Write(b)
			buf.Write(term)
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldBeNil)
			So(tc.eof, ShouldBeFalse)
			So(tc.headers[0], ShouldEqual, "header")
			So(tc.bodies[0], ShouldEqual, "body0")
			So(tc.bodies[1], ShouldEqual, "body1")
			So(tc.bodies[2], ShouldEqual, "body2")

			err = parser.ReadBlock(tc)
			So(err, ShouldEqual, io.EOF)
			So(tc.eof, ShouldBeFalse)
		})
		Convey("an incorrect terminator should cause an error", func() {
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
		Convey("an empty block should result in EOF", func() {
			buf := bytes.Buffer{}
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldEqual, io.EOF)
			So(tc.eof, ShouldBeFalse)
		})
		Convey("an error comming from the consumer should propigate through the parser", func() {
			tc.eof = true
			buf := bytes.Buffer{}
			parser.In = &buf
			err := parser.ReadAllBlocks(tc)
			So(err.Error(), ShouldContainSubstring, "double end")
		})
		Convey("a partial block should result in a non-EOF error", func() {
			buf := bytes.Buffer{}
			b, _ := bson.Marshal(strStruct{"header"})
			buf.Write(b)
			b, _ = bson.Marshal(strStruct{"body"})
			buf.Write(b)
			parser.In = &buf
			err := parser.ReadBlock(tc)
			So(err, ShouldNotBeNil)
			So(tc.eof, ShouldBeFalse)
			So(tc.headers[0], ShouldEqual, "header")
			So(tc.bodies[0], ShouldEqual, "body")
		})
		Convey("a block with a missing terminator shoud result in a non-EOF error", func() {
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
			So(tc.headers[0], ShouldEqual, "header")
			So(tc.bodies, ShouldBeNil)
		})
	})
	return
}
