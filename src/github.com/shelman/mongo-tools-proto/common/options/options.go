package options

import (
	"flag"
	"fmt"
)

type MongoToolOptions struct {
	// Print help and exit
	Help bool

	// How verbose to be
	V1 bool
	V2 bool
	V3 bool
	V4 bool
	V5 bool

	// Suppress output
	Quiet bool

	// Print version
	Version bool

	// Host to connect to
	Host string

	// Port to use
	Port string

	// Specified database and collection
	DB         string
	Collection string

	// Specify authentication credentials and database
	Username string
	Password string

	// Extra tool-specific options that can be specified by calling
	// AddOptions
	ExtraOptions

	// Bookkeeping for filtering on database and collection
	FilterNS       string // the full namespace for filtering
	FilterOnlyColl bool   // filter only on collection
	FilterBoth     bool   // filter on both db and collection
}

func (self *MongoToolOptions) Usage() {
	fmt.Println("blah blah blah usage")
	self.ExtraOptions.Usage()
}

type ExtraOptions interface {
	Register()
	PostParse() error
	Validate() error
	Usage()
}

func (self *MongoToolOptions) AddOptions(opts ExtraOptions) {
	opts.Register()
	self.ExtraOptions = opts
}

// Register the command line flags to be parsed into the options
func GetMongoToolOptions() *MongoToolOptions {

	// options bound to the command line flags
	options := &MongoToolOptions{}

	flag.BoolVar(&(options.Help), "help", false, "Return information on the"+
		" options and usage of mongotop")

	flag.BoolVar(&(options.V1), "verbose", false, "Verbosity level 1")
	flag.BoolVar(&(options.V1), "v", false, "Verbosity level 1")
	flag.BoolVar(&(options.V2), "vv", false, "Verbosity level 2")
	flag.BoolVar(&(options.V3), "vvv", false, "Verbosity level 3")
	flag.BoolVar(&(options.V4), "vvvv", false, "Verbosity level 4")
	flag.BoolVar(&(options.V5), "vvvvv", false, "Verbosity level 5")

	flag.BoolVar(&(options.Quiet), "quiet", false, "Runs the mongotop in a"+
		" quiet mode that attempts to limit the amount of output")

	flag.BoolVar(&(options.Version), "version", false, "Returns the mongotop"+
		" release number")

	flag.StringVar(&(options.Host), "host", "127.0.0.1:27017", "Specifies a"+
		" resolvable hostname for the mongod to which to connect")
	flag.StringVar(&(options.Host), "h", "127.0.0.1:27017", "Specifies a"+
		" resolvable hostname for the mongod to which to connect")

	flag.StringVar(&(options.Port), "port", "", "Specifies the TCP port"+
		" on which the MongoDB instance listens for client connections")

	flag.StringVar(&(options.DB), "db", "", "Filter by database")
	flag.StringVar(&(options.DB), "d", "", "Filter by database")

	flag.StringVar(&(options.Collection), "collection", "",
		"Filter by collection")
	flag.StringVar(&(options.Collection), "c", "", "Filter by collection")

	flag.StringVar(&(options.Username), "username", "", "Specify username for"+
		" authentication")
	flag.StringVar(&(options.Username), "u", "", "Specify username for"+
		" authentication")

	flag.StringVar(&(options.Password), "password", "", "Specify password for"+
		" authentication")
	flag.StringVar(&(options.Password), "p", "", "Specify password for"+
		" authentication")

	return options

}

// Parse the command line args into the mongo options
func (self *MongoToolOptions) ParseAndValidate() error {
	flag.Parse()

	// run post-parse logic
	if err := self.PostParse(); err != nil {
		return fmt.Errorf("error post-processing tool params: %v", err)
	}

	// run validation logic
	if err := self.Validate(); err != nil {
		return fmt.Errorf("validating tool params failed: %v", err)
	}

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
