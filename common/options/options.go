// Package options implements command-line options that are used by all of
// the mongo tools.
package options

import (
	"fmt"
	"github.com/jessevdk/go-flags"
	"github.com/mongodb/mongo-tools/common/log"
	"os"
	"runtime"
	"strconv"
)

const (
	VersionStr = "2.8.0-rc2"
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
	*Kerberos
	*Namespace
	*HiddenOptions

	//Force direct connection to the server and disable the
	//drivers automatic repl set discovery logic.
	Direct bool

	// for caching the parser
	parser *flags.Parser
}

type HiddenOptions struct {
	MaxProcs       int
	BulkWriters    int
	BulkBufferSize int

	// Specifies the number of threads to use in processing data read from the input source
	NumDecodingWorkers int

	// Specifies the number of threads to use in sending processed data over to the server
	NumInsertionWorkers int

	TempUsersColl *string
	TempRolesColl *string
}

type Namespace struct {
	// Specified database and collection
	DB         string `short:"d" long:"db" description:"database to use"`
	Collection string `short:"c" long:"collection" description:"collection to use"`
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

func (v Verbosity) Level() int {
	return len(v.Verbose)
}

func (v Verbosity) IsQuiet() bool {
	return v.Quiet
}

// Struct holding connection-related options
type Connection struct {
	Host string `short:"h" long:"host" description:"Specify a resolvable hostname to which to connect"`
	Port string `long:"port" description:"Specify the tcp port on which the mongod is listening"`
}

// Struct holding ssl-related options
type SSL struct {
	UseSSL              bool   `long:"ssl" description:"Enable connection to a mongod or mongos that has ssl enabled"`
	SSLCAFile           string `long:"sslCAFile" description:"Specify the .pem file containing the root certificate chain from the certificate authority"`
	SSLPEMKeyFile       string `long:"sslPEMKeyFile" description:"Specify the .pem file containing the certificate and key"`
	SSLPEMKeyPassword   string `long:"sslPEMKeyPassword" description:"Specify the password to decrypt the sslPEMKeyFile, if necessary"`
	SSLCRLFile          string `long:"sslCRLFile" description:"Specify the .pem file containing the certificate revocation list"`
	SSLAllowInvalidCert bool   `long:"sslAllowInvalidCertificates" description:"Bypass the validation for server certificates"`
	SSLAllowInvalidHost bool   `long:"sslAllowInvalidHosts" description:"Bypass the validation for server name"`
	SSLFipsMode         bool   `long:"sslFIPSMode" description:"Use FIPS mode of the installed openssl library"`
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

type OptionRegistrationFunction func(self *ToolOptions) error

var ConnectionOptFunctions []OptionRegistrationFunction

type EnabledOptions struct {
	Auth       bool
	Connection bool
	Namespace  bool
}

// Ask for a new instance of tool options
func New(appName, usageStr string, enabled EnabledOptions) *ToolOptions {
	hiddenOpts := &HiddenOptions{
		BulkWriters:    1,
		BulkBufferSize: 10000,
	}

	opts := &ToolOptions{
		AppName:    appName,
		VersionStr: VersionStr,
		UsageStr:   usageStr,

		General:       &General{},
		Verbosity:     &Verbosity{},
		Connection:    &Connection{},
		SSL:           &SSL{},
		Auth:          &Auth{},
		Namespace:     &Namespace{},
		HiddenOptions: hiddenOpts,
		Kerberos:      &Kerberos{},
		parser:        flags.NewNamedParser(appName, flags.None),
	}

	opts.parser.UnknownOptionHandler = func(option string, arg flags.SplitArgument, args []string) ([]string, error) {
		return parseHiddenOption(hiddenOpts, option, arg, args)
	}

	if _, err := opts.parser.AddGroup("general options", "", opts.General); err != nil {
		panic(fmt.Errorf("couldn't register general options: %v", err))
	}
	if _, err := opts.parser.AddGroup("verbosity options", "", opts.Verbosity); err != nil {
		panic(fmt.Errorf("couldn't register verbosity options: %v", err))
	}

	if enabled.Connection {
		if _, err := opts.parser.AddGroup("connection options", "", opts.Connection); err != nil {
			panic(fmt.Errorf("couldn't register connection options: %v", err))
		}

		//Register options that were enabled at compile time with build tags (ssl, sasl)
		for _, optionRegistrationFunction := range ConnectionOptFunctions {
			if err := optionRegistrationFunction(opts); err != nil {
				panic(fmt.Errorf("couldn't register command-line options: %v", err))
			}
		}
	}

	if enabled.Auth {
		if _, err := opts.parser.AddGroup("authentication options", "", opts.Auth); err != nil {
			panic(fmt.Errorf("couldn't register auth options"))
		}
	}
	if enabled.Namespace {
		if _, err := opts.parser.AddGroup("namespace options", "", opts.Namespace); err != nil {
			panic(fmt.Errorf("couldn't register namespace options"))
		}
	}

	if opts.MaxProcs <= 0 {
		opts.MaxProcs = runtime.NumCPU()
	}
	log.Logf(log.Info, "Setting num cpus to %v", opts.MaxProcs)
	runtime.GOMAXPROCS(opts.MaxProcs)
	return opts
}

// Print the usage message for the tool to stdout.  Returns whether or not the
// help flag is specified.
func (self *ToolOptions) PrintHelp(force bool) bool {
	if self.Help || force {
		self.parser.WriteHelp(os.Stdout)
	}
	return self.Help
}

// Print the tool version to stdout.  Returns whether or not the version flag
// is specified.
func (self *ToolOptions) PrintVersion() bool {
	if self.Version {
		fmt.Printf("%v version: %v\n", self.AppName, self.VersionStr)
	}
	return self.Version
}

// Interface for extra options that need to be used by specific tools
type ExtraOptions interface {
	// Name specifying what type of options these are
	Name() string
}

// Get the authentication database to use. Should be the value of
// --authenticationDatabase if it's provided, otherwise, the database that's
// specified in the tool's --db arg.
func (self *ToolOptions) GetAuthenticationDatabase() string {
	if self.Auth.Source != "" {
		return self.Auth.Source
	} else if self.Namespace != nil && self.Namespace.DB != "" {
		return self.Namespace.DB
	}
	return ""
}

// AddOptions registers an additional options group to this instance
func (self *ToolOptions) AddOptions(opts ExtraOptions) error {
	_, err := self.parser.AddGroup(opts.Name()+" options", "", opts)
	if err != nil {
		return fmt.Errorf("error setting command line options for"+
			" %v: %v", opts.Name(), err)
	}
	return nil
}

// Parse the command line args.  Returns any extra args not accounted for by
// parsing, as well as an error if the parsing returns an error.
func (self *ToolOptions) Parse() ([]string, error) {
	return self.parser.Parse()
}

func parseHiddenOption(opts *HiddenOptions, option string, arg flags.SplitArgument, args []string) ([]string, error) {
	if option == "dbpath" || option == "directoryperdb" || option == "journal" {
		return args, fmt.Errorf(`--dbpath and related flags are not supported in 2.8 tools.
See http://dochub.mongodb.org/core/tools-dbpath-deprecated for more information`)
	}

	if option == "tempUsersColl" {
		opts.TempUsersColl = new(string)
		value, consumeVal, err := getStringArg(arg, args)
		if err != nil {
			return args, fmt.Errorf("couldn't parse flag tempUsersColl: ", err)
		}
		*opts.TempUsersColl = value
		if consumeVal {
			return args[1:], nil
		}
		return args, nil
	}
	if option == "tempRolesColl" {
		opts.TempRolesColl = new(string)
		value, consumeVal, err := getStringArg(arg, args)
		if err != nil {
			return args, fmt.Errorf("couldn't parse flag tempRolesColl: ", err)
		}
		*opts.TempRolesColl = value
		if consumeVal {
			return args[1:], nil
		}
		return args, nil
	}

	var err error
	optionValue, consumeVal, err := getIntArg(arg, args)
	switch option {
	case "numThreads":
		opts.MaxProcs = optionValue
	case "numInsertionWorkersPerCollection":
		opts.BulkWriters = optionValue
	case "batchSize":
		opts.BulkBufferSize = optionValue
	case "numDecodingWorkers":
		opts.NumDecodingWorkers = optionValue
	case "numInsertionWorkers":
		opts.NumInsertionWorkers = optionValue
	default:
		return args, fmt.Errorf(`unknown option "%v"`, option)
	}
	if err != nil {
		return args, fmt.Errorf(`error parsing value for "%v": %v`, option, err)
	}
	if consumeVal {
		return args[1:], nil
	}
	return args, nil
}

//getInt returns 3 args: the parsed int value, a bool set to true if a value
//was consumed from the incoming args array during parsing, and an error
//value if parsing failed
func getIntArg(arg flags.SplitArgument, args []string) (int, bool, error) {
	var rawVal string
	consumeValue := false
	rawVal, hasVal := arg.Value()
	if !hasVal {
		if len(args) == 0 {
			return 0, false, fmt.Errorf("no value specified")
		}
		rawVal = args[0]
		consumeValue = true
	}
	val, err := strconv.Atoi(rawVal)
	if err != nil {
		return val, consumeValue, fmt.Errorf("expected an integer value but got '%v'", rawVal)
	}
	return val, consumeValue, nil
}

//getStringArg returns 3 args: the parsed string value, a bool set to true if a value
//was consumed from the incoming args array during parsing, and an error
//value if parsing failed
func getStringArg(arg flags.SplitArgument, args []string) (string, bool, error) {
	value, hasVal := arg.Value()
	if hasVal {
		return value, false, nil
	}
	if len(args) == 0 {
		return "", false, fmt.Errorf("no value specified")
	}
	return args[0], true, nil
}
