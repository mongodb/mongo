package db

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"labix.org/v2/mgo"
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
