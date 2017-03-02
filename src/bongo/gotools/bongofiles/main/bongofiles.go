// Main package for the bongofiles tool.
package main

import (
	"fmt"
	"github.com/bongodb/bongo-tools/common/db"
	"github.com/bongodb/bongo-tools/common/log"
	"github.com/bongodb/bongo-tools/common/options"
	"github.com/bongodb/bongo-tools/common/signals"
	"github.com/bongodb/bongo-tools/common/util"
	"github.com/bongodb/bongo-tools/bongofiles"
	"os"
)

func main() {
	// initialize command-line opts
	opts := options.New("bongofiles", bongofiles.Usage, options.EnabledOptions{Auth: true, Connection: true, Namespace: false})

	storageOpts := &bongofiles.StorageOptions{}
	opts.AddOptions(storageOpts)
	inputOpts := &bongofiles.InputOptions{}
	opts.AddOptions(inputOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'bongofiles --help' for more information")
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

	// add the specified database to the namespace options struct
	opts.Namespace.DB = storageOpts.DB

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")
	opts.ReplicaSetName = setName

	// create a session provider to connect to the db
	provider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}
	defer provider.Close()
	mf := bongofiles.BongoFiles{
		ToolOptions:     opts,
		StorageOptions:  storageOpts,
		SessionProvider: provider,
		InputOptions:    inputOpts,
	}

	if err := mf.ValidateCommand(args); err != nil {
		log.Logvf(log.Always, "%v", err)
		log.Logvf(log.Always, "try 'bongofiles --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	output, err := mf.Run(true)
	if err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
	fmt.Printf("%s", output)
}
