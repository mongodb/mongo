package ssl

import (
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

// Does not need a mongod to be running.  Only tests configuring the connector,
// without actually connecting.
func TestConfigureSSLConnector(t *testing.T) {

	testutil.VerifyTestType(t, "ssl")

	Convey("With an ssl db connector", t, func() {

		var connector *SSLDBConnector

		Convey("calling Configure should populate the addrs and dial timeout"+
			" appropriately, and should create a dialing function", func() {

			connector = &SSLDBConnector{}

			opts := &options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: "27017",
				},
				SSL: &options.SSL{
					UseSSL: true,
				},
				Auth: &options.Auth{},
			}
			So(connector.Configure(opts), ShouldBeNil)
			So(connector.dialInfo.Addrs, ShouldResemble,
				[]string{"localhost:27017"})
			So(connector.dialInfo.Timeout, ShouldResemble,
				DefaultSSLDialTimeout)
			So(connector.dialInfo.DialServer, ShouldNotBeNil)

		})

	})

}

// Relies on a mongod running on port 20000, with --sslCAFile and
// --sslPEMKeyFile defined.
func TestBidirectionalSSL(t *testing.T) {

	testutil.VerifyTestType(t, "ssl")

	Convey("When running ssl with bidirectional auth", t, func() {

		var connector *SSLDBConnector

		Convey("without the correct ca file to verify the server, connection"+
			" should fail", func() {

			connector = &SSLDBConnector{}

			opts := &options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: "20000",
				},
				SSL: &options.SSL{
					UseSSL:        true,
					SSLPEMKeyFile: "testdata/server.pem",
				},
			}
			So(connector.Configure(opts), ShouldBeNil)
			session, err := connector.GetNewSession()
			So(session, ShouldBeNil)
			So(err, ShouldNotBeNil)

		})

		Convey("without a valid client certificate, connection should"+
			" fail", func() {

			connector = &SSLDBConnector{}

			opts := &options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: "20000",
				},
				SSL: &options.SSL{
					UseSSL:    true,
					SSLCAFile: "testdata/ca.pem",
				},
			}
			So(connector.Configure(opts), ShouldBeNil)
			session, err := connector.GetNewSession()
			So(session, ShouldBeNil)
			So(err, ShouldNotBeNil)

		})

		Convey("with a ca file that validates the server, and a valid client"+
			" certificate, connection should work", func() {

			connector = &SSLDBConnector{}

			opts := &options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: "20000",
				},
				SSL: &options.SSL{
					UseSSL:        true,
					SSLCAFile:     "testdata/ca.pem",
					SSLPEMKeyFile: "testdata/server.pem",
				},
			}
			So(connector.Configure(opts), ShouldBeNil)
			session, err := connector.GetNewSession()
			So(session, ShouldNotBeNil)
			So(err, ShouldBeNil)
			session.Close()

		})

		Convey("without a ca file that validates the server, but with invalid"+
			" certificates allowed, connection should work", func() {

			connector = &SSLDBConnector{}

			opts := &options.ToolOptions{
				Connection: &options.Connection{
					Host: "localhost",
					Port: "20000",
				},
				SSL: &options.SSL{
					UseSSL:          true,
					SSLPEMKeyFile:   "testdata/server.pem",
					SSLAllowInvalid: true,
				},
			}
			So(connector.Configure(opts), ShouldBeNil)
			session, err := connector.GetNewSession()
			So(session, ShouldNotBeNil)
			So(err, ShouldBeNil)
			session.Close()

		})

	})

}
