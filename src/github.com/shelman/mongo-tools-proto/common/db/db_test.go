package db

import (
	"github.com/shelman/mongo-tools-proto/common/options"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestDBConnection(t *testing.T) {

	Convey("When connecting to a mongod", t, func() {

		Convey("if a session is requested before the connection is"+
			" configured, an error should be returned", func() {

			session, err := GetSession()
			So(session, ShouldBeNil)
			So(err, ShouldNotBeNil)

		})

		Convey("when running without auth", func() {

			Convey("a correctly configured session should be able to"+
				" connect", func() {

				opts := &options.MongoToolOptions{}
				opts.Host = "localhost"
				So(Configure(opts), ShouldBeNil)

				session, err := GetSession()
				So(session, ShouldNotBeNil)
				session.Close()
				So(err, ShouldBeNil)

			})

		})

	})

}
