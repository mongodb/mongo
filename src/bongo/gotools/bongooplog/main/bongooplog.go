// Main package for the bongooplog tool.
package main

import (
	"github.com/bongodb/bongo-tools/common/db"
	"github.com/bongodb/bongo-tools/common/log"
	"github.com/bongodb/bongo-tools/common/options"
	"github.com/bongodb/bongo-tools/common/signals"
	"github.com/bongodb/bongo-tools/common/util"
	"github.com/bongodb/bongo-tools/bongooplog"
	"os"
)

func main() {
	// initialize command line options
	opts := options.New("bongooplog", bongooplog.Usage,
		options.EnabledOptions{Auth: true, Connection: true, Namespace: false})

	// add the bongooplog-specific options
	sourceOpts := &bongooplog.SourceOptions{}
	opts.AddOptions(sourceOpts)

	log.Logvf(log.Always, "warning: bongooplog is deprecated, and will be removed completely in a future release")

	// parse the command line options
	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'bongooplog --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	if len(args) != 0 {
		log.Logvf(log.Always, "positional arguments not allowed: %v", args)
		log.Logvf(log.Always, "try 'bongooplog --help' for more information")
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

	// init logger
	log.SetVerbosity(opts.Verbosity)
	signals.Handle()

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")
	opts.ReplicaSetName = setName

	// validate the bongooplog options
	if sourceOpts.From == "" {
		log.Logvf(log.Always, "command line error: need to specify --from")
		os.Exit(util.ExitBadOptions)
	}

	// create a session provider for the destination server
	sessionProviderTo, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to destination host: %v", err)
		os.Exit(util.ExitError)
	}
	defer sessionProviderTo.Close()

	// create a session provider for the source server
	opts.Connection.Host = sourceOpts.From
	opts.Connection.Port = ""
	sessionProviderFrom, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to source host: %v", err)
		os.Exit(util.ExitError)
	}
	defer sessionProviderFrom.Close()

	// initialize bongooplog
	oplog := bongooplog.BongoOplog{
		ToolOptions:         opts,
		SourceOptions:       sourceOpts,
		SessionProviderFrom: sessionProviderFrom,
		SessionProviderTo:   sessionProviderTo,
	}

	// kick it off
	if err := oplog.Run(); err != nil {
		log.Logvf(log.Always, "error: %v", err)
		os.Exit(util.ExitError)
	}

}
