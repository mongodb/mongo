package db

import (
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestVanillaDBConnector(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("With a vanilla db connector", t, func() {

		var connector *VanillaDBConnector

		Convey("calling Configure should populate the addrs and dial timeout"+
			" appropriately with no error", func() {

			connector = &VanillaDBConnector{}

			opts := &options.ToolOptions{
				Connection: &options.Connection{
					Host: "host1,host2",
					Port: "20000",
				},
			}
			So(connector.Configure(opts), ShouldBeNil)
			So(connector.dialInfo.Addrs, ShouldResemble,
				[]string{"host1:20000", "host2:20000"})
			So(connector.dialInfo.Timeout, ShouldResemble, DefaultDialTimeout)

		})

		Convey("calling GetNewSession with a running mongod should connect"+
			" successfully", func() {

			connector = &VanillaDBConnector{}

			opts := &options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: "27017",
				},
			}
			So(connector.Configure(opts), ShouldBeNil)

			session, err := connector.GetNewSession()
			So(err, ShouldBeNil)
			So(session, ShouldNotBeNil)
			session.Close()

		})

	})

}
