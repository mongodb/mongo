package db

import (
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"reflect"
	"testing"
)

func TestInitSessionProvider(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("When initializing a session provider", t, func() {

		Convey("with the standard options, a provider with a standard"+
			" connector should be returned", func() {
			opts := options.ToolOptions{
				Connection: &options.Connection{},
				SSL:        &options.SSL{},
				Auth:       &options.Auth{},
			}
			provider, err := InitSessionProvider(opts)
			So(err, ShouldBeNil)
			So(reflect.TypeOf(provider.connector), ShouldEqual,
				reflect.TypeOf(&VanillaDBConnector{}))

		})

		Convey("the master session should be successfully "+
			" initialized", func() {
			opts := options.ToolOptions{
				Connection: &options.Connection{},
				SSL:        &options.SSL{},
				Auth:       &options.Auth{},
			}
			provider, err := InitSessionProvider(opts)
			So(err, ShouldBeNil)
			So(provider.masterSession, ShouldNotBeNil)

		})

	})

}

type listDatabasesCommand struct {
	Databases []map[string]interface{} `json:"databases"`
	Ok        bool                     `json:"ok"`
}

func (self *listDatabasesCommand) AsRunnable() interface{} {
	return "listDatabases"
}

func TestRunCommand(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("When running a db command against a live mongod", t, func() {

		Convey("the specified command should be run and unmarshalled into the"+
			" provided struct", func() {

			opts := options.ToolOptions{
				Connection: &options.Connection{},
				SSL:        &options.SSL{},
				Auth:       &options.Auth{},
			}
			provider, err := InitSessionProvider(opts)
			So(err, ShouldBeNil)

			cmd := &listDatabasesCommand{}
			So(provider.RunCommand("admin", cmd), ShouldBeNil)
			So(cmd.Databases, ShouldNotBeNil)
			So(cmd.Ok, ShouldBeTrue)

		})

	})

}
