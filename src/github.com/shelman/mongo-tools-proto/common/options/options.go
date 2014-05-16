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

	ExtraOptions
}

func (self *MongoToolOptions) Usage() {
	fmt.Println("blah blah blah usage")
	self.ExtraOptions.Usage()
}

type ExtraOptions interface {
	Register()
	PostParse() error
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

	flag.StringVar(&(options.Host), "host", "127.0.0.1", "Specifies a"+
		" resolvable hostname for the mongod to which to connect")
	flag.StringVar(&(options.Host), "h", "127.0.0.1", "Specifies a"+
		" resolvable hostname for the mongod to which to connect")

	flag.StringVar(&(options.Port), "port", "27017", "Specifies the TCP port"+
		" on which the MongoDB instance listens for client connections")
	flag.StringVar(&(options.Port), "p", "", "Specifies the TCP port on"+
		" which the MongoDB instance listens for client connections")

	return options

}

// Parse the command line args into the mongo options
func (self *MongoToolOptions) Parse() error {
	flag.Parse()
	if err := self.ExtraOptions.PostParse(); err != nil {
		return fmt.Errorf("error executing post-processing of params: %v", err)
	}
	return nil
}
