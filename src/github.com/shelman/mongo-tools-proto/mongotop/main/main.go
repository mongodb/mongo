package main

import (
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongotop"
	"github.com/shelman/mongo-tools-proto/mongotop/options"
	"github.com/shelman/mongo-tools-proto/mongotop/output"
	"time"
)

const (
	DEFAULT_SLEEP_TIME = 1 * time.Second
)

func main() {

	// initialize command-line opts
	opts := commonopts.New("mongotop", "0.0.1", "<options> <sleeptime>")

	// add mongotop-specific options
	outputOpts := &options.Output{}
	opts.AddOptions(outputOpts)

	_, err := opts.Parse()
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

	// create a session provider to connect to the db
	sessionProvider, err := db.InitSessionProvider(opts)
	if err != nil {
		util.Panicf("error initializing database session: %v", err)
	}

	// instantiate a mongotop instance
	top := &mongotop.MongoTop{
		Options:         opts,
		Outputter:       &output.TerminalOutputter{},
		SessionProvider: sessionProvider,
	}

	// kick it off
	if err := top.Run(); err != nil {
		util.Panicf("error running mongotop: %v", err)
	}
}
