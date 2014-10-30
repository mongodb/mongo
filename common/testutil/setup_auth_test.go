package testutil

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestSetup(t *testing.T) {
	Convey("Set up auth", t, func() {
		ssl := GetSSLOptions()
		sessionProvider, err := db.InitSessionProvider(options.ToolOptions{
			Connection: &options.Connection{
				Host: "localhost",
				Port: "27017",
			},
			Auth: &options.Auth{},
			SSL:  &ssl,
		})
		So(err, ShouldBeNil)
		session, err := sessionProvider.GetSession()
		So(err, ShouldBeNil)

		err = CreateUserAdmin(session)
		So(err, ShouldBeNil)
		err = session.DB("admin").Login(
			"uAdmin",
			"password",
		)
		So(err, ShouldBeNil)
		err = session.DB("admin").Run(
			bson.D{
				{"createUser", "passwordIsTaco"},
				{"pwd", "Taco"},
				{"roles", []bson.M{
					bson.M{
						"role": "__system",
						"db":   "admin",
					},
				}},
			},
			&bson.M{},
		)
		So(err, ShouldBeNil)
	})
}
