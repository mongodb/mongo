// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestParseConnectionString(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When extracting the replica set and hosts from a connection"+
		" url", t, func() {

		Convey("an empty url should lead to an empty replica set name"+
			" and hosts slice", func() {
			hosts, setName := ParseConnectionString("")
			So(hosts, ShouldResemble, []string{""})
			So(setName, ShouldEqual, "")
		})

		Convey("a url not specifying a replica set name should lead to"+
			" an empty replica set name", func() {
			hosts, setName := ParseConnectionString("host1,host2")
			So(hosts, ShouldResemble, []string{"host1", "host2"})
			So(setName, ShouldEqual, "")
		})

		Convey("a url specifying a replica set name should lead to that name"+
			" being returned", func() {
			hosts, setName := ParseConnectionString("foo/host1,host2")
			So(hosts, ShouldResemble, []string{"host1", "host2"})
			So(setName, ShouldEqual, "foo")
		})

	})

}

func TestCreateConnectionAddrs(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When creating the slice of connection addresses", t, func() {

		Convey("if no port is specified, the addresses should all appear"+
			" unmodified in the result", func() {

			addrs := CreateConnectionAddrs("host1,host2", "")
			So(addrs, ShouldResemble, []string{"host1", "host2"})

		})

		Convey("if a port is specified, it should be appended to each host"+
			" from the host connection string", func() {

			addrs := CreateConnectionAddrs("host1,host2", "20000")
			So(addrs, ShouldResemble, []string{"host1:20000", "host2:20000"})

		})

	})

}

func TestInvalidNames(t *testing.T) {

	Convey("Checking some invalid collection names, ", t, func() {
		Convey("test.col$ is invalid", func() {
			So(ValidateDBName("test"), ShouldBeNil)
			So(ValidateCollectionName("col$"), ShouldNotBeNil)
			So(ValidateFullNamespace("test.col$"), ShouldNotBeNil)
		})
		Convey("db/aaa.col is invalid", func() {
			So(ValidateDBName("db/aaa"), ShouldNotBeNil)
			So(ValidateCollectionName("col"), ShouldBeNil)
			So(ValidateFullNamespace("db/aaa.col"), ShouldNotBeNil)
		})
		Convey("db. is invalid", func() {
			So(ValidateDBName("db"), ShouldBeNil)
			So(ValidateCollectionName(""), ShouldNotBeNil)
			So(ValidateFullNamespace("db."), ShouldNotBeNil)
		})
		Convey("db space.col is invalid", func() {
			So(ValidateDBName("db space"), ShouldNotBeNil)
			So(ValidateCollectionName("col"), ShouldBeNil)
			So(ValidateFullNamespace("db space.col"), ShouldNotBeNil)
		})
		Convey("db x$x is invalid", func() {
			So(ValidateDBName("x$x"), ShouldNotBeNil)
			So(ValidateFullNamespace("x$x.y"), ShouldNotBeNil)
		})
		Convey("[null].[null] is invalid", func() {
			So(ValidateDBName("\x00"), ShouldNotBeNil)
			So(ValidateCollectionName("\x00"), ShouldNotBeNil)
			So(ValidateFullNamespace("\x00.\x00"), ShouldNotBeNil)
		})
		Convey("[empty] is invalid", func() {
			So(ValidateFullNamespace(""), ShouldNotBeNil)
		})
		Convey("db.col is valid", func() {
			So(ValidateDBName("db"), ShouldBeNil)
			So(ValidateCollectionName("col"), ShouldBeNil)
			So(ValidateFullNamespace("db.col"), ShouldBeNil)
		})

	})

}
