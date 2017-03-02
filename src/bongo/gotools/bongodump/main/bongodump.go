// Main package for the bongodump tool.
package main

import (
	"os"
	"time"

	"github.com/bongodb/bongo-tools/common/log"
	"github.com/bongodb/bongo-tools/common/options"
	"github.com/bongodb/bongo-tools/common/progress"
	"github.com/bongodb/bongo-tools/common/signals"
	"github.com/bongodb/bongo-tools/common/util"
	"github.com/bongodb/bongo-tools/bongodump"
)

const (
	progressBarLength   = 24
	progressBarWaitTime = time.Second * 3
)

func main() {
	// initialize command-line opts
	opts := options.New("bongodump", bongodump.Usage, options.EnabledOptions{true, true, true})

	inputOpts := &bongodump.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &bongodump.OutputOptions{}
	opts.AddOptions(outputOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'bongodump --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	if len(args) > 0 {
		log.Logvf(log.Always, "positional arguments not allowed: %v", args)
		log.Logvf(log.Always, "try 'bongodump --help' for more information")
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
	opts.ReplicaSetName = setName

	// kick off the progress bar manager
	progressManager := progress.NewBarWriter(log.Writer(0), progressBarWaitTime, progressBarLength, false)
	progressManager.Start()
	defer progressManager.Stop()

	dump := bongodump.BongoDump{
		ToolOptions:     opts,
		OutputOptions:   outputOpts,
		InputOptions:    inputOpts,
		ProgressManager: progressManager,
	}

	finishedChan := signals.HandleWithInterrupt(dump.HandleInterrupt)
	defer close(finishedChan)

	if err = dump.Init(); err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}

	if err = dump.Dump(); err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		if err == util.ErrTerminated {
			os.Exit(util.ExitKill)
		}
		os.Exit(util.ExitError)
	}
}
