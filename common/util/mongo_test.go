package util

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

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

func TestParseHost(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When parsing a host string into the contained"+
		" addresses", t, func() {

		Convey("a string with a single hostname should return a slice of just"+
			" the hostname", func() {

			addrs := parseHost("localhost")
			So(addrs, ShouldResemble, []string{"localhost"})

		})

		Convey("a string with multiple hostnames should return a slice of"+
			" all of them", func() {

			addrs := parseHost("host1,host2,host3")
			So(addrs, ShouldResemble, []string{"host1", "host2", "host3"})

		})

		Convey("a string with multiple hostnames and a replica set should"+
			" return a slice of all the host names", func() {

			addrs := parseHost("foo/host1,host2,host3")
			So(addrs, ShouldResemble, []string{"host1", "host2", "host3"})

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
