package main

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/mongotop"
	"github.com/shelman/mongo-tools-proto/mongotop/options"
	"github.com/shelman/mongo-tools-proto/mongotop/output"
	"github.com/shelman/mongo-tools-proto/mongotop/poll"
)

func main() {

	// register command-line options, and add mongotop-specific options
	opts := commonopts.GetMongoToolOptions()
	topOpts := &options.MongoTopOptions{}
	opts.AddOptions(topOpts)

	// parse the options
	err := opts.ParseAndValidate()
	if err != nil {
		fmt.Println(fmt.Sprintf("Error: %v", err))
	}

	if opts.Help {
		opts.Usage()
		return
	}

	if opts.Version {
		fmt.Println("should print mongotop version")
		return
	}

	// we're going to do real work.  configure the db connection
	db.Configure(opts)

	// instantiate a mongotop instance, and kick it off
	top := &mongotop.MongoTop{
		Options:    opts,
		TopOptions: topOpts,
		Poller:     &poll.DBPoller{},
		Outputter:  &output.TerminalOutputter{},
	}
	top.Run()
}
