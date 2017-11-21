// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"reflect"
	"testing"
)

func TestMaxInt(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When finding the maximum of two ints", t, func() {

		Convey("the larger int should be returned", func() {

			So(MaxInt(1, 2), ShouldEqual, 2)
			So(MaxInt(2, 1), ShouldEqual, 2)

		})

	})
}

func TestNumberConverter(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("With a number converter for float32", t, func() {
		floatConverter := newNumberConverter(reflect.TypeOf(float32(0)))

		Convey("numeric values should be convertable", func() {
			out, err := floatConverter(21)
			So(err, ShouldEqual, nil)
			So(out, ShouldEqual, 21.0)
			out, err = floatConverter(uint64(21))
			So(err, ShouldEqual, nil)
			So(out, ShouldEqual, 21.0)
			out, err = floatConverter(float64(27.52))
			So(err, ShouldEqual, nil)
			So(out, ShouldEqual, 27.52)
		})

		Convey("non-numeric values should fail", func() {
			_, err := floatConverter("I AM A STRING")
			So(err, ShouldNotBeNil)
			_, err = floatConverter(struct{ int }{12})
			So(err, ShouldNotBeNil)
			_, err = floatConverter(nil)
			So(err, ShouldNotBeNil)
		})
	})
}

func TestUInt32Converter(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("With a series of test values, conversions should pass", t, func() {
		out, err := ToUInt32(int64(99))
		So(err, ShouldEqual, nil)
		So(out, ShouldEqual, uint32(99))
		out, err = ToUInt32(int32(99))
		So(err, ShouldEqual, nil)
		So(out, ShouldEqual, uint32(99))
		out, err = ToUInt32(float32(99))
		So(err, ShouldEqual, nil)
		So(out, ShouldEqual, uint32(99))
		out, err = ToUInt32(float64(99))
		So(err, ShouldEqual, nil)
		So(out, ShouldEqual, uint32(99))
		out, err = ToUInt32(uint64(99))
		So(err, ShouldEqual, nil)
		So(out, ShouldEqual, uint32(99))
		out, err = ToUInt32(uint32(99))
		So(err, ShouldEqual, nil)
		So(out, ShouldEqual, uint32(99))

		Convey("but non-numeric inputs will fail", func() {
			_, err = ToUInt32(nil)
			So(err, ShouldNotBeNil)
			_, err = ToUInt32("string")
			So(err, ShouldNotBeNil)
			_, err = ToUInt32([]byte{1, 2, 3, 4})
			So(err, ShouldNotBeNil)
		})
	})
}
