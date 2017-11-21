// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"math"
	"testing"
)

func TestJSTruthyValues(t *testing.T) {
	Convey("With some sample values", t, func() {
		Convey("known server code edge cases are correct", func() {
			Convey("true -> true", func() {
				So(IsTruthy(true), ShouldBeTrue)
			})
			Convey("{} -> true", func() {
				var myMap map[string]interface{}
				So(IsTruthy(myMap), ShouldBeTrue)
				myMap = map[string]interface{}{"a": 1}
				So(IsTruthy(myMap), ShouldBeTrue)
			})
			Convey("[] -> true", func() {
				var mySlice []byte
				So(IsTruthy(mySlice), ShouldBeTrue)
				mySlice = []byte{21, 12}
				So(IsTruthy(mySlice), ShouldBeTrue)
			})
			Convey(`"" -> true`, func() {
				So(IsTruthy(""), ShouldBeTrue)
			})
			Convey("false -> false", func() {
				So(IsTruthy(false), ShouldBeFalse)
			})
			Convey("0 -> false", func() {
				So(IsTruthy(0), ShouldBeFalse)
			})
			Convey("0.0 -> false", func() {
				So(IsTruthy(float64(0)), ShouldBeFalse)
			})
			Convey("nil -> false", func() {
				So(IsTruthy(nil), ShouldBeFalse)
			})
			Convey("undefined -> false", func() {
				So(IsTruthy(bson.Undefined), ShouldBeFalse)
			})
		})

		Convey("and an assortment of non-edge cases are correct", func() {
			So(IsTruthy([]int{1, 2, 3}), ShouldBeTrue)
			So(IsTruthy("true"), ShouldBeTrue)
			So(IsTruthy("false"), ShouldBeTrue)
			So(IsTruthy(25), ShouldBeTrue)
			So(IsTruthy(math.NaN()), ShouldBeTrue)
			So(IsTruthy(25.1), ShouldBeTrue)
			So(IsTruthy(struct{ A int }{A: 12}), ShouldBeTrue)
		})
	})
}
