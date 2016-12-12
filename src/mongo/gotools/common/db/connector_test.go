package db

import (
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"testing"
	"time"
)

func TestVanillaDBConnector(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("With a vanilla db connector", t, func() {

		var connector *VanillaDBConnector

		Convey("calling Configure should populate the addrs and dial timeout"+
			" appropriately with no error", func() {

			connector = &VanillaDBConnector{}

			opts := options.ToolOptions{
				Connection: &options.Connection{
					Host: "host1,host2",
					Port: "20000",
				},
				Auth: &options.Auth{},
			}
			So(connector.Configure(opts), ShouldBeNil)
			So(connector.dialInfo.Addrs, ShouldResemble,
				[]string{"host1:20000", "host2:20000"})
			So(connector.dialInfo.Timeout, ShouldResemble, time.Duration(opts.Timeout)*time.Second)

		})

		Convey("calling GetNewSession with a running mongod should connect"+
			" successfully", func() {

			connector = &VanillaDBConnector{}

			opts := options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: DefaultTestPort,
				},
				Auth: &options.Auth{},
			}
			So(connector.Configure(opts), ShouldBeNil)

			session, err := connector.GetNewSession()
			So(err, ShouldBeNil)
			So(session, ShouldNotBeNil)
			session.Close()

		})

	})

}

func TestVanillaDBConnectorWithAuth(t *testing.T) {
	testutil.VerifyTestType(t, "auth")
	session, err := mgo.Dial("localhost:33333")
	if err != nil {
		t.Fatalf("error dialing server: %v", err)
	}

	err = testutil.CreateUserAdmin(session)
	So(err, ShouldBeNil)
	err = testutil.CreateUserWithRole(session, "cAdmin", "password",
		mgo.RoleClusterAdmin, true)
	So(err, ShouldBeNil)
	session.Close()

	Convey("With a vanilla db connector and a mongod running with"+
		" auth", t, func() {

		var connector *VanillaDBConnector

		Convey("connecting without authentication should not be able"+
			" to run commands", func() {

			connector = &VanillaDBConnector{}

			opts := options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: DefaultTestPort,
				},
				Auth: &options.Auth{},
			}
			So(connector.Configure(opts), ShouldBeNil)

			session, err := connector.GetNewSession()
			So(err, ShouldBeNil)
			So(session, ShouldNotBeNil)

			So(session.DB("admin").Run("top", &struct{}{}), ShouldNotBeNil)
			session.Close()

		})

		Convey("connecting with authentication should succeed and"+
			" authenticate properly", func() {

			connector = &VanillaDBConnector{}

			opts := options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: DefaultTestPort,
				},
				Auth: &options.Auth{
					Username: "cAdmin",
					Password: "password",
				},
			}
			So(connector.Configure(opts), ShouldBeNil)

			session, err := connector.GetNewSession()
			So(err, ShouldBeNil)
			So(session, ShouldNotBeNil)

			So(session.DB("admin").Run("top", &struct{}{}), ShouldBeNil)
			session.Close()

		})

	})

}
