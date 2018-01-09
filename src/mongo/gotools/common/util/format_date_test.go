// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
    . "github.com/smartystreets/goconvey/convey"
    "testing"
)

func TestFormatDate(t *testing.T) {

    Convey("will take valid format 2006-01-02T15:04:05.000Z", t, func() {
        _, err := FormatDate("2014-01-02T15:04:05.000Z")
        So(err, ShouldBeNil)
    })


    Convey("will take valid format 2006-01-02T15:04:05Z", t, func() {
        _, err := FormatDate("2014-03-02T15:05:05Z")
        So(err, ShouldBeNil)
    })


    Convey("will take valid format 2006-01-02T15:04Z", t, func() {
        _, err := FormatDate("2014-04-02T15:04Z")
        So(err, ShouldBeNil)
    })

    Convey("will take valid format 2006-01-02T15:04-0700", t, func() {
        _, err := FormatDate("2014-04-02T15:04-0800")
        So(err, ShouldBeNil)
    })

    Convey("will take valid format 2006-01-02T15:04:05.000-0700", t, func() {
        _, err := FormatDate("2014-04-02T15:04:05.000-0600")
        So(err, ShouldBeNil)
    })


    Convey("will take valid format 2006-01-02T15:04:05-0700", t, func() {
        _, err := FormatDate("2014-04-02T15:04:05-0500")
        So(err, ShouldBeNil)
    })

    Convey("will return an error for an invalid format", t, func() {
        _, err := FormatDate("invalid string format")
        So(err, ShouldNotBeNil)
    })

}

