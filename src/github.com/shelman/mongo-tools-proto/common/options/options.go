package options

import (
	//	"flag"
	//	"fmt"
	"github.com/shelman/mongo-tools-proto/common/util"
	"os"
	//	flag "github.com/ogier/pflag"
	"github.com/jessevdk/go-flags"
)

type MongoToolOptions struct {
	*GeneralOptions
	*VerbosityOptions
	*ConnectionOptions
	*SSLOptions
	*AuthOptions

	////////

	// Specified database and collection
	DB         string
	Collection string

	AppName    string
	VersionStr string

	// TODO below: kill this?

	// Extra tool-specific options that can be specified by calling
	// AddOptions
	ExtraOptions

	// Bookkeeping for filtering on database and collection
	FilterNS       string // the full namespace for filtering
	FilterOnlyColl bool   // filter only on collection
	FilterBoth     bool   // filter on both db and collection

	// for caching the parser
	parser *flags.Parser
}

type GeneralOptions struct {
	Help    bool `long:"help" description:"Print usage"`
	Version bool `long:"version" description:"Print the version"`
}

type VerbosityOptions struct {
	Verbose []bool `short:"v" long:"verbose" description:"Set verbosity level"`
	Quiet   bool   `long:"quiet" description:"Run in quiet mode, attempting to limit the amount of output"`
}

type ConnectionOptions struct {
	Host string `short:"h" long:"host" description:"Specify a resolvable hostname to which to connect" default:"localhost"`
	Port string `long:"port" description:"Specify the tcp port on which the mongod is listening" default:"27017"`
	IPV6 bool   `long:"ipv6" description:"Enable ipv6 support"`
}

type SSLOptions struct {
	SSL               bool   `long:"ssl" description:"Enable connection to a mongod or mongos that has ssl enabled"`
	SSLCAFile         string `long:"sslCAFile" description:"Specify the .pem file containing the root certificate chain from the certificate authority"`
	SSLPEMKeyFile     string `long:"sslPEMKeyFile" description:"Specify the .pem file containing the certificate and key"`
	SSLPEMKeyPassword string `long:"sslPEMKeyPassword" description:"Specify the password to decrypt the sslPEMKeyFile, if necessary"`
	SSLCRLFile        string `long:"sslCRLFile" description:"Specify the .pem file containing the certificate revocation list"`
	SSLAllowInvalid   bool   `long:"sslAllowInvalidCertificates" description:"Bypass the validation for server certificates"`
	SSLFipsMode       bool   `long:"sslFIPSMode" description:"Use FIPS mode of the installed openssl library"`
}

type AuthOptions struct {
	Username      string `short:"u" long:"username" description:"Specify a user name for authentication"`
	Password      string `short:"p" long:"password" description:"Specify a password for authentication"`
	AuthDB        string `long:"authenticationDatabase" description:"Specify the database that holds the user's credentials"`
	AuthMechanism string `long:"authenticationMechanism" description:"Specify the authentication mechanism to be used"`
}

type ExtraOptions interface {
	PostParse() error
	Validate() error
}

func (self *MongoToolOptions) AddOptions(opts ExtraOptions) {
	self.ExtraOptions = opts
}

// Register the command line flags to be parsed into the options
func GetMongoToolOptions(appName, versionStr string) *MongoToolOptions {

	// options bound to the command line flags
	options := &MongoToolOptions{
		AppName:    appName,
		VersionStr: versionStr,

		GeneralOptions:    &GeneralOptions{},
		VerbosityOptions:  &VerbosityOptions{},
		ConnectionOptions: &ConnectionOptions{},
		SSLOptions:        &SSLOptions{},
		AuthOptions:       &AuthOptions{},
	}

	return options
}

func (self *MongoToolOptions) PrintHelp() bool {
	if self.Help {
		self.parser.WriteHelp(os.Stdout)
	}
	return self.Help
}

func (self *MongoToolOptions) PrintVersion() bool {
	if self.Version {
		util.Printlnf("%v version: %v", self.AppName, self.VersionStr)
	}
	return self.Version
}

// Parse the command line args into the mongo options
func (self *MongoToolOptions) ParseAndValidate() error {

	// init a parser for the flags
	self.parser = flags.NewNamedParser(self.AppName, flags.None)
	self.parser.Usage = "<options> <sleeptime>"

	// register self to receive the flags
	_, err := self.parser.AddGroup("general options", "", self.GeneralOptions)
	_, err = self.parser.AddGroup("verbosity options", "", self.VerbosityOptions)
	_, err = self.parser.AddGroup("connection options", "", self.ConnectionOptions)
	_, err = self.parser.AddGroup("ssl options", "", self.SSLOptions)
	_, err = self.parser.AddGroup("authentication options", "", self.AuthOptions)

	// parse
	_, err = self.parser.Parse()
	if err != nil {
		return err
	}

	/*
		// run post-parse logic
		if err := self.PostParse(); err != nil {
			return fmt.Errorf("error post-processing tool params: %v", err)
		}

		// run validation logic
		if err := self.Validate(); err != nil {
			return fmt.Errorf("validating tool params failed: %v", err)
		}
	*/

	return nil
}

// Run the post-parse logic
func (self *MongoToolOptions) PostParse() error {
	// build the filter string and options based on the db and collection
	// specified, if any
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

	return nil
}

// Run the validation logic
func (self *MongoToolOptions) Validate() error {

	if self.ExtraOptions != nil {
		if err := self.ExtraOptions.Validate(); err != nil {
			return err
		}
	}

	return nil
}
