// Main package for the mongotop tool.
package main

import (
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongotop"
	"github.com/mongodb/mongo-tools/mongotop/options"
	"github.com/mongodb/mongo-tools/mongotop/output"
	"strconv"
	"time"
)

const (
	// the default sleep time, in seconds
	DEFAULT_SLEEP_TIME = 1
)

func main() {

	// initialize command-line opts
	opts := commonopts.New("mongotop", "0.0.1", "<options> <sleeptime>")

	// add mongotop-specific options
	outputOpts := &options.Output{}
	opts.AddOptions(outputOpts)

	extra, err := opts.Parse()
	if err != nil {
		util.Panicf("error parsing command line options: %v", err)
	}

	// print help, if specified
	if opts.PrintHelp() {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// pull out the sleeptime
	// TODO: validate args length
	sleeptime := DEFAULT_SLEEP_TIME
	if len(extra) > 0 {
		sleeptime, err = strconv.Atoi(extra[0])
		if err != nil {
			util.Panicf("bad sleep time: %v", extra[0])
		}
	}

	// create a session provider to connect to the db
	sessionProvider, err := db.InitSessionProvider(*opts)
	if err != nil {
		util.Panicf("error initializing database session: %v", err)
	}

	// instantiate a mongotop instance
	top := &mongotop.MongoTop{
		Options:         opts,
		OutputOptions:   outputOpts,
		Outputter:       &output.TerminalOutputter{},
		SessionProvider: sessionProvider,
		Sleeptime:       time.Duration(sleeptime) * time.Second,
		Once:            outputOpts.Once,
	}

	// kick it off
	if err := top.Run(); err != nil {
		util.Panicf("error running mongotop: %v", err)
	}
}
