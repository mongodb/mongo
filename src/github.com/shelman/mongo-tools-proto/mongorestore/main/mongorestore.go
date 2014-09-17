package main

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	"github.com/shelman/mongo-tools-proto/common/log"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongorestore"
	"github.com/shelman/mongo-tools-proto/mongorestore/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongorestore", "0.0.1", "<options>")
	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &options.OutputOptions{}
	opts.AddOptions(outputOpts)

	args, err := opts.Parse()
	if err != nil {
		fmt.Printf("error parsing command line options: %v\n\n", err)
		fmt.Printf("try 'mongorestore --help' for more information\n")
		os.Exit(2)
	}

	// print help or version info, if specified
	if opts.PrintHelp() {
		return
	}
	if opts.PrintVersion() {
		return
	}

	targetDir := ""
	if len(args) == 0 {
		targetDir = "dump"
	} else {
		targetDir = args[0]
		if len(args) > 1 {
			fmt.Printf("error parsing command line: too many arguments\n")
			os.Exit(1)
		}
	}

	// init logger
	log.InitToolLogger(opts.Verbosity)

	// create a session provider to connect to the db
	sessionProvider, err := db.InitSessionProvider(*opts)
	if err != nil {
		fmt.Printf("error initializing database session: %v\n", err)
		os.Exit(1) //TODO copy legacy exit code
	}

	restore := mongorestore.MongoRestore{
		ToolOptions:     opts,
		OutputOptions:   outputOpts,
		InputOptions:    inputOpts,
		SessionProvider: sessionProvider,
		TargetDirectory: targetDir,
	}

	err = restore.Restore()
	if err != nil {
		util.Exitf(1, "%v", err)
	}

}
