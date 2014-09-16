// Package options implements command-line options that are used by all of
// the mongo tools.
package options

import (
	"fmt"
	"github.com/jessevdk/go-flags"
	"github.com/shelman/mongo-tools-proto/common/util"
	"os"
)

// Struct encompassing all of the options that are reused across tools: "help",
// "version", verbosity settings, ssl settings, etc.
type ToolOptions struct {

	// The name of the tool
	AppName string

	// The version of the tool
	VersionStr string

	// String describing usage, not including the tool name
	UsageStr string

	// Sub-option types
	*General
	*Verbosity
	*Connection
	*SSL
	*Auth
	*Namespace
	*Kerberos

	// Extra tool-specific options that can be specified by calling
	// AddOptions
	Extra []ExtraOptions

	// for caching the parser
	parser *flags.Parser

	////////

	// TODO below: kill this?

	// Bookkeeping for filtering on database and collection
	FilterNS       string // the full namespace for filtering
	FilterOnlyColl bool   // filter only on collection
	FilterBoth     bool   // filter on both db and collection
}

type Namespace struct {
	// Specified database and collection
	DB         string `short:"d" long:"db" description:"database to use"`
	Collection string `short:"c" long:"collection" description:"collection to use"`

	// DBPath for direct-storage interface
	DBPath string `long:"dbpath"`
}

// Struct holding generic options
type General struct {
	Help    bool `long:"help" description:"Print usage"`
	Version bool `long:"version" description:"Print the version"`
}

// Struct holding verbosity-related options
type Verbosity struct {
	Verbose []bool `short:"v" long:"verbose" description:"Set verbosity level"`
	Quiet   bool   `long:"quiet" description:"Run in quiet mode, attempting to limit the amount of output"`
}

// Struct holding connection-related options
type Connection struct {
	Host string `short:"h" long:"host" description:"Specify a resolvable hostname to which to connect" default:"localhost:27017"`
	Port string `long:"port" description:"Specify the tcp port on which the mongod is listening"`
}

// Struct holding ssl-related options
type SSL struct {
	UseSSL            bool   `long:"ssl" description:"Enable connection to a mongod or mongos that has ssl enabled"`
	SSLCAFile         string `long:"sslCAFile" description:"Specify the .pem file containing the root certificate chain from the certificate authority"`
	SSLPEMKeyFile     string `long:"sslPEMKeyFile" description:"Specify the .pem file containing the certificate and key"`
	SSLPEMKeyPassword string `long:"sslPEMKeyPassword" description:"Specify the password to decrypt the sslPEMKeyFile, if necessary"`
	SSLCRLFile        string `long:"sslCRLFile" description:"Specify the .pem file containing the certificate revocation list"`
	SSLAllowInvalid   bool   `long:"sslAllowInvalidCertificates" description:"Bypass the validation for server certificates"`
	SSLFipsMode       bool   `long:"sslFIPSMode" description:"Use FIPS mode of the installed openssl library"`
}

// Struct holding auth-related options
type Auth struct {
	Username  string `short:"u" long:"username" description:"Specify a user name for authentication"`
	Password  string `short:"p" long:"password" description:"Specify a password for authentication"`
	Source    string `long:"authenticationDatabase" description:"Specify the database that holds the user's credentials"`
	Mechanism string `long:"authenticationMechanism" description:"Specify the authentication mechanism to be used"`
}

// Struct for Kerberos/GSSAPI-specific options
type Kerberos struct {
	Service     string `long:"gssapiServiceName" description:"Service name to use when authenticating using GSSAPI/Kerberos ('mongodb' by default)"`
	ServiceHost string `long:"gssapiHostName" description:"Hostname to use when authenticating using GSSAPI/Kerberos (remote server's address by default)"`
}

// Ask for a new instance of tool options
func New(appName, versionStr, usageStr string) *ToolOptions {
	return &ToolOptions{
		AppName:    appName,
		VersionStr: versionStr,
		UsageStr:   usageStr,

		General:    &General{},
		Verbosity:  &Verbosity{},
		Connection: &Connection{},
		SSL:        &SSL{},
		Auth:       &Auth{},
		Namespace:  &Namespace{},
		Kerberos:   &Kerberos{},
	}
}

// Print the usage message for the tool to stdout.  Returns whether or not the
// help flag is specified.
func (self *ToolOptions) PrintHelp() bool {
	if self.Help {
		self.parser.WriteHelp(os.Stdout)
	}
	return self.Help
}

// Print the tool version to stdout.  Returns whether or not the version flag
// is specified.
func (self *ToolOptions) PrintVersion() bool {
	if self.Version {
		util.Printlnf("%v version: %v", self.AppName, self.VersionStr)
	}
	return self.Version
}

// Interface for extra options that need to be used by specific tools
type ExtraOptions interface {
	// Name specifying what type of options these are
	Name() string
}

// Add extra command line options into the tool options
func (self *ToolOptions) AddOptions(opts ExtraOptions) {
	self.Extra = append(self.Extra, opts)
}

// Parse the command line args.  Returns any extra args not accounted for by
// parsing, as well as an error if the parsing returns an error.
func (self *ToolOptions) Parse() ([]string, error) {

	// init a parser for the flags
	self.parser = flags.NewNamedParser(self.AppName, flags.None)
	self.parser.Usage = self.UsageStr

	// register self to receive the flags
	_, err := self.parser.AddGroup("general options", "", self.General)
	_, err = self.parser.AddGroup("verbosity options", "", self.Verbosity)
	_, err = self.parser.AddGroup("connection options", "", self.Connection)
	_, err = self.parser.AddGroup("ssl options", "", self.SSL)
	_, err = self.parser.AddGroup("authentication options", "", self.Auth)
	_, err = self.parser.AddGroup("namespace options", "", self.Namespace)
	_, err = self.parser.AddGroup("kerberos options", "", self.Kerberos)

	// register all of the extra options
	for _, eo := range self.Extra {
		_, err = self.parser.AddGroup(eo.Name()+" options", "", eo)
		if err != nil {
			return nil, fmt.Errorf("error setting command line options for"+
				" %v: %v", eo.Name(), err)
		}
	}

	// parse
	return self.parser.Parse()
}

////////////

// Run the post-parse logic
func (self *ToolOptions) PostParse() error {
	// build the filter string and options based on the db and collection
	// specified, if any

	/*
		if self.DB != "" {
			self.FilterNS = self.DB + "."
			if self.Collection != "" {
				self.FilterBoth = true
				self.FilterNS += self.Collection
			}
		} else if self.Collection != "" {
			self.FilterOnlyColl = true
			self.FilterNS = "." + self.Collection
		}

		// post-parse the extra params
		if self.ExtraOptions != nil {
			if err := self.ExtraOptions.PostParse(); err != nil {
				return err
			}
		}
	*/

	return nil
}

// Run the validation logic
func (self *ToolOptions) Validate() error {
	/*
		if self.ExtraOptions != nil {
			if err := self.ExtraOptions.Validate(); err != nil {
				return err
			}
		}
	*/

	return nil
}
