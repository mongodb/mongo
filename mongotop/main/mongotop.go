// Main package for the mongotop tool.
package main

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongotop"
	"github.com/mongodb/mongo-tools/mongotop/options"
	"os"
	"strconv"
	"time"
)

func main() {

	// initialize command-line opts
	opts := commonopts.New("mongotop", "<options> <sleeptime>",
		commonopts.EnabledOptions{Auth: true, Connection: true, Namespace: false})

	// add mongotop-specific options
	outputOpts := &options.Output{}
	opts.AddOptions(outputOpts)

	extra, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v", err)
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

	if len(extra) > 1 {
		log.Logf(log.Always, "too many positional arguments")
		opts.PrintHelp(true)
		os.Exit(util.ExitBadOptions)
	}

	sleeptime := 1 // default to 1 second sleep time
	if len(extra) > 0 {
		sleeptime, err = strconv.Atoi(extra[0])
		if err != nil || sleeptime <= 0 {
			log.Logf(log.Always, "bad sleep time: %v", extra[0])
			os.Exit(util.ExitBadOptions)
		}
	}
	if outputOpts.RowCount < 0 {
		log.Logf(log.Always, "invalid value for row count: %v", outputOpts.RowCount)
		os.Exit(util.ExitBadOptions)
	}

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")

	// create a session provider to connect to the db
	sessionProvider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logf(log.Always, "error connecting to host: %v\n", err)
		os.Exit(util.ExitError)
	}

	// instantiate a mongotop instance
	top := &mongotop.MongoTop{
		Options:         opts,
		OutputOptions:   outputOpts,
		SessionProvider: sessionProvider,
		Sleeptime:       time.Duration(sleeptime) * time.Second,
	}

	// kick it off
	if err := top.Run(); err != nil {
		log.Logf(log.Always, "error running mongotop: %v", err)
		os.Exit(util.ExitError)
	}
}
