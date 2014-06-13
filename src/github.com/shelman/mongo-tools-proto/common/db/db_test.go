package db

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db/ssl"
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"labix.org/v2/mgo"
	"reflect"
	"testing"
)

type ErrorConnector struct{}

func (self *ErrorConnector) Configure(opts *options.ToolOptions) error {
	return fmt.Errorf("Configure() error")
}

func (self *ErrorConnector) GetNewSession() (*mgo.Session, error) {
	return nil, fmt.Errorf("GetNewSession() error")
}

type SuccessConnector struct{}

func (self *SuccessConnector) Configure(opts *options.ToolOptions) error {
	return nil
}

func (self *SuccessConnector) GetNewSession() (*mgo.Session, error) {
	dialInfo := &mgo.DialInfo{Addrs: []string{"localhost:27017"}}
	return mgo.DialWithInfo(dialInfo)
}

func TestGetSession(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("With a session provider", t, func() {

		var provider *SessionProvider

		Convey("when calling GetSession", func() {

			Convey("if the embedded DBConnector cannot reach the database,"+
				" an error should be returned", func() {

				provider = &SessionProvider{
					connector: &ErrorConnector{},
				}
				session, err := provider.GetSession()
				So(session, ShouldBeNil)
				So(err, ShouldNotBeNil)

			})

			Convey("if there is no master session initialized, a new one"+
				" should be created and copied", func() {

				provider = &SessionProvider{
					connector: &SuccessConnector{},
				}
				session, err := provider.GetSession()
				So(err, ShouldBeNil)
				So(session, ShouldNotBeNil)
				session.Close()
				So(provider.masterSession, ShouldNotBeNil)
				provider.masterSession.Close()

			})

			Convey("if a master session exists, it should be copied to create"+
				" a new session", func() {

				provider = &SessionProvider{
					connector: &SuccessConnector{},
				}
				for i := 0; i < 5; i++ {
					session, err := provider.GetSession()
					So(session, ShouldNotBeNil)
					So(err, ShouldBeNil)
					session.Close()
				}
				provider.masterSession.Close()

			})

		})

	})

}

func TestInitSessionProvider(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("When initializing a session provider", t, func() {

		Convey("if nil options are passed in, an error should be"+
			" returned", func() {

			provider, err := InitSessionProvider(nil)
			So(err, ShouldNotBeNil)
			So(provider, ShouldBeNil)

		})

		Convey("if the options passed in specify ssl, a provider with an ssl"+
			" connector should be returned", func() {

			opts := &options.ToolOptions{
				Connection: &options.Connection{},
				SSL: &options.SSL{
					UseSSL: true,
				},
			}
			provider, err := InitSessionProvider(opts)
			So(err, ShouldBeNil)
			So(reflect.TypeOf(provider.connector), ShouldEqual,
				reflect.TypeOf(&ssl.SSLDBConnector{}))

		})

		Convey("otherwise, a provider with a standard connector should be"+
			" returned", func() {
			opts := &options.ToolOptions{
				Connection: &options.Connection{},
				SSL:        &options.SSL{},
			}
			provider, err := InitSessionProvider(opts)
			So(err, ShouldBeNil)
			So(reflect.TypeOf(provider.connector), ShouldEqual,
				reflect.TypeOf(&VanillaDBConnector{}))

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

			opts := &options.ToolOptions{
				Connection: &options.Connection{},
				SSL:        &options.SSL{},
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
