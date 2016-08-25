// Package options implements command-line options that are used by all of
// the mongo tools.
package options

import (
	"github.com/jessevdk/go-flags"
	"github.com/mongodb/mongo-tools/common/log"

	"fmt"
	"os"
	"regexp"
	"runtime"
	"strconv"
	"strings"
)

// Gitspec that the tool was built with. Needs to be set using -ldflags
var (
	VersionStr = "built-without-version-string"
	Gitspec    = "built-without-git-spec"
)

// Struct encompassing all of the options that are reused across tools: "help",
// "version", verbosity settings, ssl settings, etc.
type ToolOptions struct {

	// The name of the tool
	AppName string

	// The version of the tool
	VersionStr string

	// Sub-option types
	*General
	*Verbosity
	*Connection
	*SSL
	*Auth
	*Kerberos
	*Namespace

	// Force direct connection to the server and disable the
	// drivers automatic repl set discovery logic.
	Direct bool

	// ReplicaSetName, if specified, will prevent the obtained session from
	// communicating with any server which is not part of a replica set
	// with the given name. The default is to communicate with any server
	// specified or discovered via the servers contacted.
	ReplicaSetName string

	// for caching the parser
	parser *flags.Parser
}

type Namespace struct {
	// Specified database and collection
	DB         string `short:"d" long:"db" value-name:"<database-name>" description:"database to use"`
	Collection string `short:"c" long:"collection" value-name:"<collection-name>" description:"collection to use"`
}

// Struct holding generic options
type General struct {
	Help    bool `long:"help" description:"print usage"`
	Version bool `long:"version" description:"print the tool version and exit"`

	MaxProcs int `long:"numThreads" default:"0" hidden:"true"`
}

// Struct holding verbosity-related options
type Verbosity struct {
	SetVerbosity func(string) `short:"v" long:"verbose" value-name:"<level>" description:"more detailed log output (include multiple times for more verbosity, e.g. -vvvvv, or specify a numeric value, e.g. --verbose=N)" optional:"true" optional-value:""`
	Quiet        bool         `long:"quiet" description:"hide all log output"`
	VLevel       int          `no-flag:"true"`
}

func (v Verbosity) Level() int {
	return v.VLevel
}

func (v Verbosity) IsQuiet() bool {
	return v.Quiet
}

// Struct holding connection-related options
type Connection struct {
	Host string `short:"h" long:"host" value-name:"<hostname>" description:"mongodb host to connect to (setname/host1,host2 for replica sets)"`
	Port string `long:"port" value-name:"<port>" description:"server port (can also use --host hostname:port)"`

	Timeout int `long:"dialTimeout" default:"3" hidden:"true" description:"dial timeout in seconds"`
}

// Struct holding ssl-related options
type SSL struct {
	UseSSL              bool   `long:"ssl" description:"connect to a mongod or mongos that has ssl enabled"`
	SSLCAFile           string `long:"sslCAFile" value-name:"<filename>" description:"the .pem file containing the root certificate chain from the certificate authority"`
	SSLPEMKeyFile       string `long:"sslPEMKeyFile" value-name:"<filename>" description:"the .pem file containing the certificate and key"`
	SSLPEMKeyPassword   string `long:"sslPEMKeyPassword" value-name:"<password>" description:"the password to decrypt the sslPEMKeyFile, if necessary"`
	SSLCRLFile          string `long:"sslCRLFile" value-name:"<filename>" description:"the .pem file containing the certificate revocation list"`
	SSLAllowInvalidCert bool   `long:"sslAllowInvalidCertificates" description:"bypass the validation for server certificates"`
	SSLAllowInvalidHost bool   `long:"sslAllowInvalidHostnames" description:"bypass the validation for server name"`
	SSLFipsMode         bool   `long:"sslFIPSMode" description:"use FIPS mode of the installed openssl library"`
}

// Struct holding auth-related options
type Auth struct {
	Username  string `short:"u" value-name:"<username>" long:"username" description:"username for authentication"`
	Password  string `short:"p" value-name:"<password>" long:"password" description:"password for authentication"`
	Source    string `long:"authenticationDatabase" value-name:"<database-name>" description:"database that holds the user's credentials"`
	Mechanism string `long:"authenticationMechanism" value-name:"<mechanism>" description:"authentication mechanism to use"`
}

// Struct for Kerberos/GSSAPI-specific options
type Kerberos struct {
	Service     string `long:"gssapiServiceName" value-name:"<service-name>" description:"service name to use when authenticating using GSSAPI/Kerberos ('mongodb' by default)"`
	ServiceHost string `long:"gssapiHostName" value-name:"<host-name>" description:"hostname to use when authenticating using GSSAPI/Kerberos (remote server's address by default)"`
}

type OptionRegistrationFunction func(o *ToolOptions) error

var ConnectionOptFunctions []OptionRegistrationFunction

type EnabledOptions struct {
	Auth       bool
	Connection bool
	Namespace  bool
}

func parseVal(val string) int {
	idx := strings.Index(val, "=")
	ret, err := strconv.Atoi(val[idx+1:])
	if err != nil {
		panic(fmt.Errorf("value was not a valid integer: %v", err))
	}
	return ret
}

// Ask for a new instance of tool options
func New(appName, usageStr string, enabled EnabledOptions) *ToolOptions {
	opts := &ToolOptions{
		AppName:    appName,
		VersionStr: VersionStr,

		General:    &General{},
		Verbosity:  &Verbosity{},
		Connection: &Connection{},
		SSL:        &SSL{},
		Auth:       &Auth{},
		Namespace:  &Namespace{},
		Kerberos:   &Kerberos{},
		parser: flags.NewNamedParser(
			fmt.Sprintf("%v %v", appName, usageStr), flags.None),
	}

	// Called when -v or --verbose is parsed
	opts.SetVerbosity = func(val string) {
		if i, err := strconv.Atoi(val); err == nil {
			opts.VLevel = opts.VLevel + i // -v=N or --verbose=N
		} else if matched, _ := regexp.MatchString(`^v+$`, val); matched {
			opts.VLevel = opts.VLevel + len(val) + 1 // Handles the -vvv cases
		} else if matched, _ := regexp.MatchString(`^v+=[0-9]$`, val); matched {
			opts.VLevel = parseVal(val) // I.e. -vv=3
		} else if val == "" {
			opts.VLevel = opts.VLevel + 1 // Increment for every occurrence of flag
		} else {
			log.Logvf(log.Always, "Invalid verbosity value given")
			os.Exit(-1)
		}
	}

	opts.parser.UnknownOptionHandler = opts.handleUnknownOption

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

		// Register options that were enabled at compile time with build tags (ssl, sasl)
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
	log.Logvf(log.Info, "Setting num cpus to %v", opts.MaxProcs)
	runtime.GOMAXPROCS(opts.MaxProcs)
	return opts
}

// UseReadOnlyHostDescription changes the help description of the --host arg to
// not mention the shard/host:port format used in the data-mutating tools
func (o *ToolOptions) UseReadOnlyHostDescription() {
	hostOpt := o.parser.FindOptionByLongName("host")
	hostOpt.Description = "mongodb host(s) to connect to (use commas to delimit hosts)"
}

// FindOptionByLongName finds an option in any of the added option groups by
// matching its long name; useful for modifying the attributes (e.g. description
// or name) of an option
func (o *ToolOptions) FindOptionByLongName(name string) *flags.Option {
	return o.parser.FindOptionByLongName(name)
}

// Print the usage message for the tool to stdout.  Returns whether or not the
// help flag is specified.
func (o *ToolOptions) PrintHelp(force bool) bool {
	if o.Help || force {
		o.parser.WriteHelp(os.Stdout)
	}
	return o.Help
}

type versionInfo struct {
	key, value string
}

var versionInfos []versionInfo

// Print the tool version to stdout.  Returns whether or not the version flag
// is specified.
func (o *ToolOptions) PrintVersion() bool {
	if o.Version {
		fmt.Printf("%v version: %v\n", o.AppName, o.VersionStr)
		fmt.Printf("git version: %v\n", Gitspec)
		fmt.Printf("Go version: %v\n", runtime.Version())
		fmt.Printf("   os: %v\n", runtime.GOOS)
		fmt.Printf("   arch: %v\n", runtime.GOARCH)
		fmt.Printf("   compiler: %v\n", runtime.Compiler)
		for _, info := range versionInfos {
			fmt.Printf("%s: %s\n", info.key, info.value)
		}
	}
	return o.Version
}

// Interface for extra options that need to be used by specific tools
type ExtraOptions interface {
	// Name specifying what type of options these are
	Name() string
}

func (auth *Auth) RequiresExternalDB() bool {
	return auth.Mechanism == "GSSAPI" || auth.Mechanism == "PLAIN" || auth.Mechanism == "MONGODB-X509"
}

// ShouldAskForPassword returns true if the user specifies a username flag
// but no password, and the authentication mechanism requires a password.
func (auth *Auth) ShouldAskForPassword() bool {
	return auth.Username != "" && auth.Password == "" &&
		!(auth.Mechanism == "MONGODB-X509" || auth.Mechanism == "GSSAPI")
}

// Get the authentication database to use. Should be the value of
// --authenticationDatabase if it's provided, otherwise, the database that's
// specified in the tool's --db arg.
func (o *ToolOptions) GetAuthenticationDatabase() string {
	if o.Auth.Source != "" {
		return o.Auth.Source
	} else if o.Auth.RequiresExternalDB() {
		return "$external"
	} else if o.Namespace != nil && o.Namespace.DB != "" {
		return o.Namespace.DB
	}
	return ""
}

// AddOptions registers an additional options group to this instance
func (o *ToolOptions) AddOptions(opts ExtraOptions) {
	_, err := o.parser.AddGroup(opts.Name()+" options", "", opts)
	if err != nil {
		panic(fmt.Sprintf("error setting command line options for  %v: %v",
			opts.Name(), err))
	}
}

// Parse the command line args.  Returns any extra args not accounted for by
// parsing, as well as an error if the parsing returns an error.
func (o *ToolOptions) Parse() ([]string, error) {
	return o.parser.Parse()
}

func (opts *ToolOptions) handleUnknownOption(option string, arg flags.SplitArgument, args []string) ([]string, error) {
	if option == "dbpath" || option == "directoryperdb" || option == "journal" {
		return args, fmt.Errorf("--dbpath and related flags are not supported in 3.0 tools.\n" +
			"See http://dochub.mongodb.org/core/tools-dbpath-deprecated for more information")
	}

	return args, fmt.Errorf(`unknown option "%v"`, option)
}

// getIntArg returns 3 args: the parsed int value, a bool set to true if a value
// was consumed from the incoming args array during parsing, and an error
// value if parsing failed
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

// getStringArg returns 3 args: the parsed string value, a bool set to true if a value
// was consumed from the incoming args array during parsing, and an error
// value if parsing failed
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
