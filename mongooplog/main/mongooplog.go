package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongooplog"
	"github.com/mongodb/mongo-tools/mongooplog/options"
	"os"
)

func main() {

	// initialize command line options
	opts := commonopts.New("mongooplog", "<options>", commonopts.EnabledOptions{Auth: true, Connection: true, Namespace: false})

	// add the mongooplog-specific options
	sourceOpts := &options.SourceOptions{}
	opts.AddOptions(sourceOpts)

	// parse the command line options
	_, err := opts.Parse()
	if err != nil {
		fmt.Printf("error parsing command line: %v\n\n", err)
		fmt.Printf("try 'mongooplog --help' for more information\n")
		os.Exit(2)
	}

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// init logger
	log.SetVerbosity(opts.Verbosity)

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")

	// validate the mongooplog options
	if err := sourceOpts.Validate(); err != nil {
		fmt.Printf("command line error: %v\n", err)
		os.Exit(2)
	}

	// create a session provider for the destination server
	sessionProviderTo, err := db.InitSessionProvider(*opts)
	if err != nil {
		fmt.Printf("error connecting to destination host: %v", err)
		os.Exit(1)
	}

	// create a session provider for the source server
	opts.Connection.Host = sourceOpts.From
	opts.Connection.Port = ""
	sessionProviderFrom, err := db.InitSessionProvider(*opts)
	if err != nil {
		fmt.Printf("error connecting to source host: %v\n", err)
		os.Exit(1)
	}

	// initialize mongooplog
	oplog := mongooplog.MongoOplog{
		ToolOptions:         opts,
		SourceOptions:       sourceOpts,
		SessionProviderFrom: sessionProviderFrom,
		SessionProviderTo:   sessionProviderTo,
	}

	// kick it off
	if err := oplog.Run(); err != nil {
		fmt.Printf("error: %v\n", err)
		os.Exit(1)
	}

}
