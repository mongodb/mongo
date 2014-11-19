package main

import (
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongodump"
	"github.com/mongodb/mongo-tools/mongodump/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongodump", "<options>", commonopts.EnabledOptions{true, true, true})

	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &options.OutputOptions{}
	opts.AddOptions(outputOpts)

	_, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v\n\n", err)
		opts.PrintHelp(true)
		return
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

	// don't attempt to discover other members of a replica set
	opts.Direct = true

	dump := mongodump.MongoDump{
		ToolOptions:   opts,
		OutputOptions: outputOpts,
		InputOptions:  inputOpts,
	}

	err = dump.Init()
	if err != nil {
		log.Logf(log.Always, "%v", err)
		os.Exit(1)
	}

	err = dump.Dump()
	if err != nil {
		log.Logf(log.Always, "%v", err)
		os.Exit(1)
	}

}
