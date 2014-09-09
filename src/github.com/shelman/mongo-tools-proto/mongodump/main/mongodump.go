package main

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	"github.com/shelman/mongo-tools-proto/common/log"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongodump"
	"github.com/shelman/mongo-tools-proto/mongodump/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongodump", "0.0.1", "<options>")

	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &options.OutputOptions{}
	opts.AddOptions(outputOpts)

	_, err := opts.Parse()
	if err != nil {
		fmt.Printf("error parsing command line options: %v\n\n", err)
		fmt.Printf("try 'mongodump --help' for more information\n")
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

	// create a session provider to connect to the db
	sessionProvider, err := db.InitSessionProvider(opts)
	if err != nil {
		fmt.Printf("error initializing database session: %v\n", err)
		os.Exit(1) //TODO copy legacy exit code
	}

	dump := mongodump.MongoDump{
		ToolOptions:     opts,
		OutputOptions:   outputOpts,
		InputOptions:    inputOpts,
		SessionProvider: sessionProvider,
	}

	err = dump.Dump()
	if err != nil {
		util.Exitf(1, "%v", err)
	}

}
