// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package options implements command-line options that are used by all of
// the mongo tools.
package options

import (
	"fmt"
	"os"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"time"

	flags "github.com/jessevdk/go-flags"
	"github.com/mongodb/mongo-tools-common/failpoint"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/util"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/mongo/driver/connstring"
)

var (
	KnownURIOptionsAuth           = []string{"authsource", "authmechanism"}
	KnownURIOptionsConnection     = []string{"connecttimeoutms"}
	KnownURIOptionsSSL            = []string{"ssl"}
	KnownURIOptionsReadPreference = []string{"readpreference"}
	KnownURIOptionsKerberos       = []string{"gssapiservicename", "gssapihostname"}
	KnownURIOptionsWriteConcern   = []string{"wtimeout", "w", "j", "fsync"}
	KnownURIOptionsReplicaSet     = []string{"replicaset"}
)

// XXX Force these true as the Go driver supports them always.  Once the
// conditionals that depend on them are removed, these can be removed.
var (
	BuiltWithSSL    = true
	BuiltWithGSSAPI = true
)

const IncompatibleArgsErrorFormat = "illegal argument combination: cannot specify %s and --uri"
const ConflictingArgsErrorFormat = "illegal argument combination: %s conflicts with --uri"

// Struct encompassing all of the options that are reused across tools: "help",
// "version", verbosity settings, ssl settings, etc.
type ToolOptions struct {

	// The name of the tool
	AppName string

	// The version of the tool
	VersionStr string

	// The git commit reference of the tool
	GitCommit string

	// Sub-option types
	*URI
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

	// ReadPreference, if specified, sets the client default
	ReadPreference *readpref.ReadPref

	// WriteConcern, if specified, sets the client default
	WriteConcern *writeconcern.WriteConcern

	// RetryWrites, if specified, sets the client default.
	RetryWrites *bool

	// for caching the parser
	parser *flags.Parser

	// for checking which options were enabled on this tool
	enabledOptions EnabledOptions
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

	MaxProcs   int    `long:"numThreads" hidden:"true"`
	Failpoints string `long:"failpoints" hidden:"true"`
	Trace      bool   `long:"trace" hidden:"true"`
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

type URI struct {
	ConnectionString string `long:"uri" value-name:"mongodb-uri" description:"mongodb uri connection string"`

	knownURIParameters   []string
	extraOptionsRegistry []ExtraOptions
	connString           connstring.ConnString
}

// Struct holding connection-related options
type Connection struct {
	Host string `short:"h" long:"host" value-name:"<hostname>" description:"mongodb host to connect to (setname/host1,host2 for replica sets)"`
	Port string `long:"port" value-name:"<port>" description:"server port (can also use --host hostname:port)"`

	Timeout                int `long:"dialTimeout" default:"3" hidden:"true" description:"dial timeout in seconds"`
	SocketTimeout          int `long:"socketTimeout" default:"0" hidden:"true" description:"socket timeout in seconds (0 for no timeout)"`
	TCPKeepAliveSeconds    int `long:"TCPKeepAliveSeconds" default:"30" hidden:"true" description:"seconds between TCP keep alives"`
	ServerSelectionTimeout int `long:"serverSelectionTimeout" hidden:"true" description:"seconds to wait for server selection; 0 means driver default"`
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
	Service     string `long:"gssapiServiceName" value-name:"<service-name>" description:"service name to use when authenticating using GSSAPI/Kerberos (default: mongodb)"`
	ServiceHost string `long:"gssapiHostName" value-name:"<host-name>" description:"hostname to use when authenticating using GSSAPI/Kerberos (default: <remote server's address>)"`
}
type WriteConcern struct {
	// Specifies the write concern for each write operation that mongofiles writes to the target database.
	// By default, mongofiles waits for a majority of members from the replica set to respond before returning.
	WriteConcern string `long:"writeConcern" value-name:"<write-concern>" default:"majority" default-mask:"-" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}'"`

	w        int
	wtimeout int
	fsync    bool
	journal  bool
}

type OptionRegistrationFunction func(*ToolOptions) error

var ConnectionOptFunctions []OptionRegistrationFunction

type EnabledOptions struct {
	Auth       bool
	Connection bool
	Namespace  bool
	URI        bool
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
func New(appName, versionStr, gitCommit, usageStr string, enabled EnabledOptions) *ToolOptions {
	opts := &ToolOptions{
		AppName:    appName,
		VersionStr: versionStr,
		GitCommit:  gitCommit,

		General:    &General{},
		Verbosity:  &Verbosity{},
		Connection: &Connection{},
		URI:        &URI{},
		SSL:        &SSL{},
		Auth:       &Auth{},
		Namespace:  &Namespace{},
		Kerberos:   &Kerberos{},
		parser: flags.NewNamedParser(
			fmt.Sprintf("%v %v", appName, usageStr), flags.None),
		enabledOptions: enabled,
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

	opts.URI.AddKnownURIParameters(KnownURIOptionsReplicaSet)

	if _, err := opts.parser.AddGroup("general options", "", opts.General); err != nil {
		panic(fmt.Errorf("couldn't register general options: %v", err))
	}
	if _, err := opts.parser.AddGroup("verbosity options", "", opts.Verbosity); err != nil {
		panic(fmt.Errorf("couldn't register verbosity options: %v", err))
	}

	// this call disables failpoints if compiled without failpoint support
	EnableFailpoints(opts)

	if enabled.Connection {
		opts.URI.AddKnownURIParameters(KnownURIOptionsConnection)
		if _, err := opts.parser.AddGroup("connection options", "", opts.Connection); err != nil {
			panic(fmt.Errorf("couldn't register connection options: %v", err))
		}
		opts.URI.AddKnownURIParameters(KnownURIOptionsSSL)
		if _, err := opts.parser.AddGroup("ssl options", "", opts.SSL); err != nil {
			panic(fmt.Errorf("couldn't register SSL options: %v", err))
		}
	}

	if enabled.Auth {
		opts.URI.AddKnownURIParameters(KnownURIOptionsAuth)
		if _, err := opts.parser.AddGroup("authentication options", "", opts.Auth); err != nil {
			panic(fmt.Errorf("couldn't register auth options"))
		}
		opts.URI.AddKnownURIParameters(KnownURIOptionsKerberos)
		if _, err := opts.parser.AddGroup("kerberos options", "", opts.Kerberos); err != nil {
			panic(fmt.Errorf("couldn't register Kerberos options"))
		}
	}
	if enabled.Namespace {
		if _, err := opts.parser.AddGroup("namespace options", "", opts.Namespace); err != nil {
			panic(fmt.Errorf("couldn't register namespace options"))
		}
	}
	if enabled.URI {
		if _, err := opts.parser.AddGroup("uri options", "", opts.URI); err != nil {
			panic(fmt.Errorf("couldn't register URI options"))
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
func (opts *ToolOptions) UseReadOnlyHostDescription() {
	hostOpt := opts.parser.FindOptionByLongName("host")
	hostOpt.Description = "mongodb host(s) to connect to (use commas to delimit hosts)"
}

// FindOptionByLongName finds an option in any of the added option groups by
// matching its long name; useful for modifying the attributes (e.g. description
// or name) of an option
func (opts *ToolOptions) FindOptionByLongName(name string) *flags.Option {
	return opts.parser.FindOptionByLongName(name)
}

// Print the usage message for the tool to stdout.  Returns whether or not the
// help flag is specified.
func (opts *ToolOptions) PrintHelp(force bool) bool {
	if opts.Help || force {
		opts.parser.WriteHelp(os.Stdout)
	}
	return opts.Help
}

type versionInfo struct {
	key, value string
}

var versionInfos []versionInfo

// Print the tool version to stdout.  Returns whether or not the version flag
// is specified.
func (opts *ToolOptions) PrintVersion() bool {
	if opts.Version {
		fmt.Printf("%v version: %v\n", opts.AppName, opts.VersionStr)
		fmt.Printf("git version: %v\n", opts.GitCommit)
		fmt.Printf("Go version: %v\n", runtime.Version())
		fmt.Printf("   os: %v\n", runtime.GOOS)
		fmt.Printf("   arch: %v\n", runtime.GOARCH)
		fmt.Printf("   compiler: %v\n", runtime.Compiler)
		for _, info := range versionInfos {
			fmt.Printf("%s: %s\n", info.key, info.value)
		}
	}
	return opts.Version
}

// Interface for extra options that need to be used by specific tools
type ExtraOptions interface {
	// Name specifying what type of options these are
	Name() string
}

type URISetter interface {
	// SetOptionsFromURI provides a way for tools to fetch any options that were
	// set in the URI and set them on the ExtraOptions that they pass to the options
	// package.
	SetOptionsFromURI(connstring.ConnString) error
}

func (auth *Auth) RequiresExternalDB() bool {
	return auth.Mechanism == "GSSAPI" || auth.Mechanism == "PLAIN" || auth.Mechanism == "MONGODB-X509"
}

func (auth *Auth) IsSet() bool {
	return *auth != Auth{}
}

// ShouldAskForPassword returns true if the user specifies a username flag
// but no password, and the authentication mechanism requires a password.
func (auth *Auth) ShouldAskForPassword() bool {
	return auth.Username != "" && auth.Password == "" &&
		!(auth.Mechanism == "MONGODB-X509" || auth.Mechanism == "GSSAPI")
}

func NewURI(unparsed string) (*URI, error) {
	cs, err := connstring.Parse(unparsed)
	if err != nil {
		return nil, fmt.Errorf("error parsing URI from %v: %v", unparsed, err)
	}
	return &URI{ConnectionString: cs.String(), connString: cs}, nil
}

func (uri *URI) GetConnectionAddrs() []string {
	return uri.connString.Hosts
}
func (uri *URI) ParsedConnString() *connstring.ConnString {
	if uri.ConnectionString == "" {
		return nil
	}
	return &uri.connString
}
func (uri *URI) AddKnownURIParameters(uriFieldNames []string) {
	uri.knownURIParameters = append(uri.knownURIParameters, uriFieldNames...)
}

func (opts *ToolOptions) EnabledToolOptions() EnabledOptions {
	return opts.enabledOptions
}

func (uri *URI) LogUnsupportedOptions() {
	allOptionsFromURI := map[string]struct{}{}

	for optName := range uri.connString.Options {
		allOptionsFromURI[optName] = struct{}{}
	}

	for optName := range uri.connString.UnknownOptions {
		allOptionsFromURI[optName] = struct{}{}
	}

	for _, optName := range uri.knownURIParameters {
		if _, ok := allOptionsFromURI[optName]; ok {
			delete(allOptionsFromURI, optName)
		}
	}

	unsupportedOptions := make([]string, len(allOptionsFromURI))
	optionIndex := 0
	for optionName := range allOptionsFromURI {
		unsupportedOptions[optionIndex] = optionName
		optionIndex++
	}

	for _, optName := range unsupportedOptions {
		log.Logvf(log.Always, "WARNING: ignoring unsupported URI parameter '%v'", optName)
	}
}

// Get the authentication database to use. Should be the value of
// --authenticationDatabase if it's provided, otherwise, the database that's
// specified in the tool's --db arg.
func (opts *ToolOptions) GetAuthenticationDatabase() string {
	if opts.Auth.Source != "" {
		return opts.Auth.Source
	} else if opts.Auth.RequiresExternalDB() {
		return "$external"
	} else if opts.Namespace != nil && opts.Namespace.DB != "" {
		return opts.Namespace.DB
	}
	return ""
}

// AddOptions registers an additional options group to this instance
func (opts *ToolOptions) AddOptions(extraOpts ExtraOptions) {
	_, err := opts.parser.AddGroup(extraOpts.Name()+" options", "", extraOpts)
	if err != nil {
		panic(fmt.Sprintf("error setting command line options for  %v: %v",
			extraOpts.Name(), err))
	}

	if opts.enabledOptions.URI {
		opts.URI.extraOptionsRegistry = append(opts.URI.extraOptionsRegistry, extraOpts)
	}
}

// Parse the command line args.  Returns any extra args not accounted for by
// parsing, as well as an error if the parsing returns an error.
func (opts *ToolOptions) ParseArgs(args []string) ([]string, error) {
	args, err := opts.parser.ParseArgs(args)
	if err != nil {
		return []string{}, err
	}

	failpoint.ParseFailpoints(opts.Failpoints)

	err = opts.NormalizeHostPortURI()
	if err != nil {
		return []string{}, err
	}

	return args, err
}

func (opts *ToolOptions) NormalizeHostPortURI() error {
	if opts.URI != nil && opts.URI.ConnectionString != "" {
		cs, err := connstring.Parse(opts.URI.ConnectionString)
		if err != nil {
			return err
		}
		err = opts.setOptionsFromURI(cs)
		if err != nil {
			return err
		}
	} else {
		// If URI not provided, get replica set name and generate connection string
		_, opts.ReplicaSetName = util.SplitHostArg(opts.Host)
		uri, err := NewURI(util.BuildURI(opts.Host, opts.Port))
		if err != nil {
			return err
		}
		opts.URI = uri
	}

	// connect directly, unless a replica set name is explicitly specified
	opts.Direct = (opts.ReplicaSetName == "")

	return nil
}

func (opts *ToolOptions) handleUnknownOption(option string, arg flags.SplitArgument, args []string) ([]string, error) {
	if option == "dbpath" || option == "directoryperdb" || option == "journal" {
		return args, fmt.Errorf("--dbpath and related flags are not supported in 3.0 tools.\n" +
			"See http://dochub.mongodb.org/core/tools-dbpath-deprecated for more information")
	}

	return args, fmt.Errorf(`unknown option "%v"`, option)
}

func (opts *ToolOptions) setOptionsFromURI(cs connstring.ConnString) error {
	opts.URI.connString = cs

	// if Connection settings are enabled, then verify that other methods
	// of specifying weren't used and set timeout
	if opts.enabledOptions.Connection {
		switch {
		case opts.Connection.Host != "":
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--host")
		case opts.Connection.Port != "":
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--port")
		case opts.Connection.Timeout != 3:
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--dialTimeout")
		case opts.Connection.SocketTimeout != 0:
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--socketTimeout")
		}
		opts.Connection.Timeout = int(cs.ConnectTimeout / time.Millisecond)
		opts.Connection.SocketTimeout = int(cs.SocketTimeout / time.Millisecond)
	}

	if opts.enabledOptions.Auth {
		switch {
		case opts.Username != "":
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--username")
		case opts.Password != "" && cs.Password != "":
			return fmt.Errorf(IncompatibleArgsErrorFormat,
				"illegal argument combination: cannot specify password in uri and --password")
		case opts.Source != "":
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--authenticationDatabase")
		case opts.Auth.Mechanism != "":
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--authenticationMechanism")
		}
		opts.Username = cs.Username
		opts.Password = cs.Password
		// Only set Source if it's not the Go driver default; that means a user must
		// have passed an authsource option or provided a database in the URI path.
		if _, ok := cs.Options["authsource"]; ok || cs.Database != "" {
			opts.Source = cs.AuthSource
		}
		opts.Auth.Mechanism = cs.AuthMechanism
	}
	if opts.enabledOptions.Namespace {
		if opts.Namespace != nil && opts.Namespace.DB != "" {
			return fmt.Errorf(IncompatibleArgsErrorFormat, "--db")
		}
	}

	opts.Namespace.DB = cs.Database
	opts.Direct = (cs.Connect == connstring.SingleConnect)
	opts.ReplicaSetName = cs.ReplicaSet

	if cs.SSL && !BuiltWithSSL {
		if strings.HasPrefix(cs.Original, "mongodb+srv") {
			return fmt.Errorf("SSL enabled by default when using SRV but tool not built with SSL: " +
				"SSL must be explicitly disabled with ssl=false in the connection string")
		}
		return fmt.Errorf("cannot use ssl: tool not built with SSL support")
	}
	if cs.SSLSet {
		if opts.SSL.UseSSL && !cs.SSL {
			return fmt.Errorf(ConflictingArgsErrorFormat, "--ssl")
		}
		opts.SSL.UseSSL = cs.SSL
	}

	if strings.ToLower(cs.AuthMechanism) == "gssapi" {
		if !BuiltWithGSSAPI {
			return fmt.Errorf("cannot specify gssapiservicename: tool not built with kerberos support")
		}
		opts.Kerberos.Service = cs.AuthMechanismProperties["SERVICE_NAME"]
		opts.Kerberos.ServiceHost = cs.AuthMechanismProperties["SERVICE_HOST"]
	}

	for _, extraOpts := range opts.URI.extraOptionsRegistry {
		if uriSetter, ok := extraOpts.(URISetter); ok {
			err := uriSetter.SetOptionsFromURI(cs)
			if err != nil {
				return err
			}
		}
	}
	return nil
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
