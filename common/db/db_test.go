package db

import (
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
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
