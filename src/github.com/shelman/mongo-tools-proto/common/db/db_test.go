package db

import (
	"github.com/shelman/mongo-tools-proto/common/options"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestSessionProvider(t *testing.T) {

	var provider *SessionProvider
	var err error

	Convey("When using a session provider to connect to the db", t, func() {

		opts := &options.MongoToolOptions{
			Host: "localhost",
			Port: "27017",
		}
		provider, err = InitSessionProvider(opts)
		So(err, ShouldBeNil)

	})

}
