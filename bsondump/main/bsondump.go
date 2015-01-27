// Main package for the bsondump tool.
package main

import (
	"github.com/mongodb/mongo-tools/bsondump"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"os"
)

func main() {
	go signals.Handle()
	// initialize command-line opts
	opts := options.New("bsondump", bsondump.Usage, options.EnabledOptions{})
	bsonDumpOpts := &bsondump.BSONDumpOptions{}
	opts.AddOptions(bsonDumpOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v", err)
		log.Logf(log.Always, "try 'bsondump --help' for more information")
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

	// pull out the filename
	if len(args) == 0 {
		log.Logf(log.Always, "must provide a filename")
		log.Logf(log.Always, "try 'bsondump --help' for more information")
		os.Exit(util.ExitBadOptions)
	} else if len(args) > 1 {
		log.Logf(log.Always, "too many positional arguments: %v", args)
		log.Logf(log.Always, "try 'bsondump --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	dumper := bsondump.BSONDump{
		ToolOptions:     opts,
		BSONDumpOptions: bsonDumpOpts,
		FileName:        args[0],
		Out:             os.Stdout,
	}

	log.Logf(log.DebugLow, "running bsondump with --objcheck: %v", bsonDumpOpts.ObjCheck)

	if len(bsonDumpOpts.Type) != 0 && bsonDumpOpts.Type != "debug" && bsonDumpOpts.Type != "json" {
		log.Logf(log.Always, "Unsupported output type '%v'. Must be either 'debug' or 'json'", bsonDumpOpts.Type)
		os.Exit(util.ExitBadOptions)
	}

	err = dumper.Open()
	if err != nil {
		log.Logf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}

	var numFound int
	if bsonDumpOpts.Type == "debug" {
		numFound, err = dumper.Debug()
	} else {
		numFound, err = dumper.JSON()
	}

	log.Logf(log.Always, "%v objects found", numFound)
	if err != nil {
		log.Log(log.Always, err.Error())
		os.Exit(util.ExitError)
	}
}
