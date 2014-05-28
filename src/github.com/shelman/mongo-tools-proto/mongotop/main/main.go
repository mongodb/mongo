package main

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongotop"
	"github.com/shelman/mongo-tools-proto/mongotop/options"
	"github.com/shelman/mongo-tools-proto/mongotop/output"
)

func main() {

	// register command-line options, and add mongotop-specific options
	opts := commonopts.GetMongoToolOptions()
	topOpts := &options.MongoTopOptions{}
	opts.AddOptions(topOpts)

	// parse the options
	err := opts.ParseAndValidate()
	if err != nil {
		util.Panicf("error in command line options: %v", err)
	}

	// if appropriate, print help
	if opts.Help {
		opts.Usage()
		return
	}

	// if appropriate, print the version
	if opts.Version {
		fmt.Println("should print mongotop version")
		return
	}

	// create a session provider to connect to the db
	sessionProvider, err := db.InitSessionProvider(opts)
	if err != nil {
		util.Panicf("error initializing database session: %v", err)
	}

	// instantiate a mongotop instance
	top := &mongotop.MongoTop{
		Options:         opts,
		TopOptions:      topOpts,
		Outputter:       &output.TerminalOutputter{},
		SessionProvider: sessionProvider,
	}

	// kick it off
	if err := top.Run(); err != nil {
		util.Panicf("error running mongotop: %v", err)
	}

}
