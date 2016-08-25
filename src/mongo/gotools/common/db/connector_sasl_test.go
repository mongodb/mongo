// +build sasl

package db

// This file runs Kerberos tests if build with sasl is enabled

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db/kerberos"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"os"
	"runtime"
	"testing"
)

var (
	KERBEROS_HOST = "ldaptest.10gen.cc"
	KERBEROS_USER = "drivers@LDAPTEST.10GEN.CC"
)

func TestKerberosDBConnector(t *testing.T) {
	Convey("should be able to successfully connect", t, func() {
		connector := &kerberos.KerberosDBConnector{}

		opts := options.ToolOptions{
			Connection: &options.Connection{
				Host: KERBEROS_HOST,
				Port: "27017",
			},
			Auth: &options.Auth{
				Username: KERBEROS_USER,
			},
			Kerberos: &options.Kerberos{
				Service:     "mongodb",
				ServiceHost: KERBEROS_HOST,
			},
		}

		if runtime.GOOS == "windows" {
			opts.Auth.Password = os.Getenv(testutil.WinKerberosPwdEnv)
			if opts.Auth.Password == "" {
				panic(fmt.Sprintf("Need to set %v environment variable to run kerberos tests on windows",
					testutil.WinKerberosPwdEnv))
			}
		}

		So(connector.Configure(opts), ShouldBeNil)

		session, err := connector.GetNewSession()
		So(err, ShouldBeNil)
		So(session, ShouldNotBeNil)

		n, err := session.DB("kerberos").C("test").Find(bson.M{}).Count()
		So(err, ShouldBeNil)
		So(n, ShouldEqual, 1)
	})
}
