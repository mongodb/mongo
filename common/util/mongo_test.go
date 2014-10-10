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
