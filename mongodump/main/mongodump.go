package main

import (
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongodump"
	"github.com/mongodb/mongo-tools/mongodump/options"
	"os"
	"strings"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongodump", "<options>", commonopts.EnabledOptions{true, true, true})

	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &options.OutputOptions{}
	opts.AddOptions(outputOpts)

	extraArgs, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v\n\n", err)
		opts.PrintHelp(true)
		return
	}

	if len(extraArgs) > 0 {
		log.Logf(log.Always,
			"error: mongodump does not accept positional arguments (%v) see help text",
			strings.Join(extraArgs, ", "))
		opts.PrintHelp(true)
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

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")

	dump := mongodump.MongoDump{
		ToolOptions:   opts,
		OutputOptions: outputOpts,
		InputOptions:  inputOpts,
	}

	err = dump.Init()
	if err != nil {
		log.Logf(log.Always, "%v", err)
		os.Exit(util.ExitError)
	}

	err = dump.Dump()
	if err != nil {
		log.Logf(log.Always, "%v", err)
		os.Exit(util.ExitError)
	}

}
