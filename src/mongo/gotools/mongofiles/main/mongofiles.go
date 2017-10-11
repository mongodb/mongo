// Main package for the mongofiles tool.
package main

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongofiles"

	"fmt"
	"os"
)

func main() {
	// initialize command-line opts
	opts := options.New("mongofiles", mongofiles.Usage, options.EnabledOptions{Auth: true, Connection: true, Namespace: false, URI: true})

	storageOpts := &mongofiles.StorageOptions{}
	opts.AddOptions(storageOpts)
	inputOpts := &mongofiles.InputOptions{}
	opts.AddOptions(inputOpts)
	opts.URI.AddKnownURIParameters(options.KnownURIOptionsReadPreference)

	args, err := opts.ParseArgs(os.Args[1:])
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'mongofiles --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}
	log.SetVerbosity(opts.Verbosity)
	signals.Handle()

	// verify uri options and log them
	opts.URI.LogUnsupportedOptions()

	// add the specified database to the namespace options struct
	opts.Namespace.DB = storageOpts.DB

	// create a session provider to connect to the db
	provider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}
	defer provider.Close()
	mf := mongofiles.MongoFiles{
		ToolOptions:     opts,
		StorageOptions:  storageOpts,
		SessionProvider: provider,
		InputOptions:    inputOpts,
	}

	if err := mf.ValidateCommand(args); err != nil {
		log.Logvf(log.Always, "%v", err)
		log.Logvf(log.Always, "try 'mongofiles --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	output, err := mf.Run(true)
	if err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
	fmt.Printf("%s", output)
}
