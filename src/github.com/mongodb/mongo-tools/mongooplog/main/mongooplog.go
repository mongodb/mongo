package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongooplog"
	"github.com/mongodb/mongo-tools/mongooplog/options"
	"os"
)

func main() {

	// initialize command line options
	opts := commonopts.New("mongooplog", "0.0.1", "<options>")

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
	if opts.PrintHelp() {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// init logger
	log.InitToolLogger(opts.Verbosity)

	// validate the mongooplog options
	if err := sourceOpts.Validate(); err != nil {
		fmt.Printf("command line error: %v\n", err)
		os.Exit(2)
	}

	// create a command runner for the destination server
	cmdRunnerTo, err := mongooplog.CreateCommandRunner(opts)
	if err != nil {
		fmt.Printf("error connecting to destination: %v", err)
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
		CmdRunnerTo:         cmdRunnerTo,
	}

	// kick it off
	if err := oplog.Run(); err != nil {
		fmt.Printf("error: %v\n", err)
		os.Exit(1)
	}

}
