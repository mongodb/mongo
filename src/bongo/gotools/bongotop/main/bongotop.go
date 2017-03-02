// Main package for the bongotop tool.
package main

import (
	"github.com/bongodb/bongo-tools/common/db"
	"github.com/bongodb/bongo-tools/common/log"
	"github.com/bongodb/bongo-tools/common/options"
	"github.com/bongodb/bongo-tools/common/signals"
	"github.com/bongodb/bongo-tools/common/util"
	"github.com/bongodb/bongo-tools/bongotop"
	"gopkg.in/mgo.v2"
	"os"
	"strconv"
	"time"
)

func main() {
	// initialize command-line opts
	opts := options.New("bongotop", bongotop.Usage,
		options.EnabledOptions{Auth: true, Connection: true, Namespace: false})
	opts.UseReadOnlyHostDescription()

	// add bongotop-specific options
	outputOpts := &bongotop.Output{}
	opts.AddOptions(outputOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'bongotop --help' for more information")
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

	if len(args) > 1 {
		log.Logvf(log.Always, "too many positional arguments")
		log.Logvf(log.Always, "try 'bongotop --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	sleeptime := 1 // default to 1 second sleep time
	if len(args) > 0 {
		sleeptime, err = strconv.Atoi(args[0])
		if err != nil || sleeptime <= 0 {
			log.Logvf(log.Always, "invalid sleep time: %v", args[0])
			os.Exit(util.ExitBadOptions)
		}
	}
	if outputOpts.RowCount < 0 {
		log.Logvf(log.Always, "invalid value for --rowcount: %v", outputOpts.RowCount)
		os.Exit(util.ExitBadOptions)
	}

	if opts.Auth.Username != "" && opts.Auth.Source == "" && !opts.Auth.RequiresExternalDB() {
		log.Logvf(log.Always, "--authenticationDatabase is required when authenticating against a non $external database")
		os.Exit(util.ExitBadOptions)
	}

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")
	opts.ReplicaSetName = setName

	// create a session provider to connect to the db
	sessionProvider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}

	if setName == "" {
		sessionProvider.SetReadPreference(mgo.PrimaryPreferred)
	}

	// fail fast if connecting to a bongos
	isBongos, err := sessionProvider.IsBongos()
	if err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
	if isBongos {
		log.Logvf(log.Always, "cannot run bongotop against a bongos")
		os.Exit(util.ExitError)
	}

	// instantiate a bongotop instance
	top := &bongotop.BongoTop{
		Options:         opts,
		OutputOptions:   outputOpts,
		SessionProvider: sessionProvider,
		Sleeptime:       time.Duration(sleeptime) * time.Second,
	}

	// kick it off
	if err := top.Run(); err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
}
