package options

import (
	"github.com/mongodb/mongo-tools/common/connstring"
	. "github.com/smartystreets/goconvey/convey"

	"runtime"
	"testing"
	"time"
)

func TestVerbosityFlag(t *testing.T) {
	Convey("With a new ToolOptions", t, func() {
		enabled := EnabledOptions{false, false, false, false}
		optPtr := New("", "", enabled)
		So(optPtr, ShouldNotBeNil)
		So(optPtr.parser, ShouldNotBeNil)

		Convey("no verbosity flags, Level should be 0", func() {
			_, err := optPtr.parser.ParseArgs([]string{})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 0)
		})

		Convey("one short verbosity flag, Level should be 1", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-v"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 1)
		})

		Convey("three short verbosity flags (consecutive), Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-vvv"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("three short verbosity flags (dispersed), Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-v", "-v", "-v"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("short verbosity flag assigned to 3, Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-v=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("consecutive short flags with assignment, only assignment holds", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-vv=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("one long verbose flag, Level should be 1", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 1)
		})

		Convey("three long verbosity flags, Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose", "--verbose", "--verbose"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("long verbosity flag assigned to 3, Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("mixed assignment and bare flag, total is sum", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose", "--verbose=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 4)
		})
	})
}

type uriTester struct {
	Name                 string
	CS                   connstring.ConnString
	OptsIn               *ToolOptions
	OptsExpected         *ToolOptions
	WithSSL              bool
	WithGSSAPI           bool
	ShouldError          bool
	ShouldAskForPassword bool
}

func TestParseAndSetOptions(t *testing.T) {
	Convey("With a matrix of URIs and expected results", t, func() {
		enabledURIOnly := EnabledOptions{false, false, false, true}
		testCases := []uriTester{
			{
				Name: "not built with ssl",
				CS: connstring.ConnString{
					UseSSL: true,
				},
				WithSSL:      false,
				OptsIn:       New("", "", enabledURIOnly),
				OptsExpected: New("", "", enabledURIOnly),
				ShouldError:  true,
			},
			{
				Name: "built with ssl",
				CS: connstring.ConnString{
					UseSSL: true,
				},
				WithSSL: true, OptsIn: New("", "", enabledURIOnly),
				OptsExpected: &ToolOptions{
					General:    &General{},
					Verbosity:  &Verbosity{},
					Connection: &Connection{},
					URI:        &URI{},
					SSL: &SSL{
						UseSSL: true,
					},
					Auth:           &Auth{},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: enabledURIOnly,
				},
				ShouldError: false,
			},
			{
				Name: "not built with gssapi",
				CS: connstring.ConnString{
					KerberosService: "service",
				},
				WithGSSAPI:   false,
				OptsIn:       New("", "", enabledURIOnly),
				OptsExpected: New("", "", enabledURIOnly),
				ShouldError:  true,
			},
			{
				Name: "built with gssapi",
				CS: connstring.ConnString{
					KerberosService:     "service",
					KerberosServiceHost: "servicehost",
				},
				WithGSSAPI: true,
				OptsIn:     New("", "", enabledURIOnly),
				OptsExpected: &ToolOptions{
					General:    &General{},
					Verbosity:  &Verbosity{},
					Connection: &Connection{},
					URI:        &URI{},
					SSL:        &SSL{},
					Auth:       &Auth{},
					Namespace:  &Namespace{},
					Kerberos: &Kerberos{
						Service:     "service",
						ServiceHost: "servicehost",
					},
					enabledOptions: enabledURIOnly,
				},
				ShouldError: false,
			},
			{
				Name: "connection fields set",
				CS: connstring.ConnString{
					ConnectTimeout: time.Duration(100) * time.Millisecond,
				},
				OptsIn: &ToolOptions{
					General:   &General{},
					Verbosity: &Verbosity{},
					Connection: &Connection{
						Timeout: 3,
					},
					URI:            &URI{},
					SSL:            &SSL{},
					Auth:           &Auth{},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{Connection: true, URI: true},
				},
				OptsExpected: &ToolOptions{
					General:   &General{},
					Verbosity: &Verbosity{},
					Connection: &Connection{
						Timeout: 100,
					},
					URI:            &URI{},
					SSL:            &SSL{},
					Auth:           &Auth{},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{Connection: true, URI: true},
				},
				ShouldError: false,
			},
			{
				Name: "auth fields set",
				CS: connstring.ConnString{
					AuthMechanism: "MONGODB-X509",
					AuthSource:    "authSource",
					Username:      "user",
					Password:      "password",
				},
				OptsIn: &ToolOptions{
					General:        &General{},
					Verbosity:      &Verbosity{},
					Connection:     &Connection{},
					URI:            &URI{},
					SSL:            &SSL{},
					Auth:           &Auth{},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{Auth: true, URI: true},
				},
				OptsExpected: &ToolOptions{
					General:    &General{},
					Verbosity:  &Verbosity{},
					Connection: &Connection{},
					URI:        &URI{},
					SSL:        &SSL{},
					Auth: &Auth{
						Username:  "user",
						Password:  "password",
						Source:    "authSource",
						Mechanism: "MONGODB-X509",
					},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{Connection: true, URI: true},
				},
				ShouldError: false,
			},
			{
				Name: "should ask for password",
				CS: connstring.ConnString{
					AuthMechanism: "MONGODB-X509",
					AuthSource:    "authSource",
					Username:      "user",
				},
				OptsIn: &ToolOptions{
					General:        &General{},
					Verbosity:      &Verbosity{},
					Connection:     &Connection{},
					URI:            &URI{},
					SSL:            &SSL{},
					Auth:           &Auth{},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{Auth: true, URI: true},
				},
				OptsExpected: &ToolOptions{
					General:    &General{},
					Verbosity:  &Verbosity{},
					Connection: &Connection{},
					URI:        &URI{},
					SSL:        &SSL{},
					Auth: &Auth{
						Username:  "user",
						Source:    "authSource",
						Mechanism: "MONGODB-X509",
					},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{Connection: true, URI: true},
				},
				ShouldError:          false,
				ShouldAskForPassword: true,
			},
			{
				Name: "single connect sets 'Direct'",
				CS: connstring.ConnString{
					Connect: connstring.SingleConnect,
				},
				OptsIn: New("", "", enabledURIOnly),
				OptsExpected: &ToolOptions{
					General:        &General{},
					Verbosity:      &Verbosity{},
					Connection:     &Connection{},
					URI:            &URI{},
					SSL:            &SSL{},
					Auth:           &Auth{},
					Direct:         true,
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{URI: true},
				},
				ShouldError: false,
			},
			{
				Name: "ReplSetName is set when CS contains it",
				CS: connstring.ConnString{
					ReplicaSet: "replset",
				},
				OptsIn: New("", "", enabledURIOnly),
				OptsExpected: &ToolOptions{
					General:        &General{},
					Verbosity:      &Verbosity{},
					Connection:     &Connection{},
					URI:            &URI{},
					SSL:            &SSL{},
					Auth:           &Auth{},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{URI: true},
					ReplicaSetName: "replset",
				},
				ShouldError: false,
			},
			{
				Name: "fail when uri and options set",
				CS: connstring.ConnString{
					Hosts: []string{"host"},
				},
				OptsIn: &ToolOptions{
					General:   &General{},
					Verbosity: &Verbosity{},
					Connection: &Connection{
						Host: "host",
					},
					URI:            &URI{},
					SSL:            &SSL{},
					Auth:           &Auth{},
					Namespace:      &Namespace{},
					Kerberos:       &Kerberos{},
					enabledOptions: EnabledOptions{Connection: true, URI: true},
				},
				OptsExpected: New("", "", EnabledOptions{Connection: true, URI: true}),
				ShouldError:  true,
			},
		}

		Convey("results should match expected", func() {
			for _, testCase := range testCases {
				t.Log("Test Case:", testCase.Name)

				testCase.OptsIn.URI.ConnectionString = "mongodb://dummy"
				testCase.OptsExpected.URI.ConnectionString = "mongodb://dummy"

				BuiltWithSSL = testCase.WithSSL
				BuiltWithGSSAPI = testCase.WithGSSAPI

				testCase.OptsIn.URI.connString = testCase.CS

				err := testCase.OptsIn.setOptionsFromURI(testCase.CS)

				if testCase.ShouldError {
					So(err, ShouldNotBeNil)
				} else {
					So(err, ShouldBeNil)
				}

				So(testCase.OptsIn.Connection.Timeout, ShouldResemble, testCase.OptsExpected.Connection.Timeout)
				So(testCase.OptsIn.Username, ShouldResemble, testCase.OptsExpected.Username)
				So(testCase.OptsIn.Password, ShouldResemble, testCase.OptsExpected.Password)
				So(testCase.OptsIn.Source, ShouldResemble, testCase.OptsExpected.Source)
				So(testCase.OptsIn.Auth.Mechanism, ShouldResemble, testCase.OptsExpected.Auth.Mechanism)
				So(testCase.OptsIn.Direct, ShouldResemble, testCase.OptsExpected.Direct)
				So(testCase.OptsIn.ReplicaSetName, ShouldResemble, testCase.OptsExpected.ReplicaSetName)
				So(testCase.OptsIn.SSL.UseSSL, ShouldResemble, testCase.OptsExpected.SSL.UseSSL)
				So(testCase.OptsIn.Kerberos.Service, ShouldResemble, testCase.OptsExpected.Kerberos.Service)
				So(testCase.OptsIn.Kerberos.ServiceHost, ShouldResemble, testCase.OptsExpected.Kerberos.ServiceHost)
				So(testCase.OptsIn.Auth.ShouldAskForPassword(), ShouldEqual, testCase.OptsIn.ShouldAskForPassword())
			}
		})
	})
}

// Regression test for TOOLS-1694 to prevent issue from TOOLS-1115
func TestHiddenOptionsDefaults(t *testing.T) {
	Convey("With a ToolOptions parsed", t, func() {
		enabled := EnabledOptions{Connection: true}
		opts := New("", "", enabled)
		_, err := opts.parser.ParseArgs([]string{})
		So(err, ShouldBeNil)
		Convey("hidden options should have expected values", func() {
			So(opts.MaxProcs, ShouldEqual, runtime.NumCPU())
			So(opts.Timeout, ShouldEqual, 3)
		})
	})

}
