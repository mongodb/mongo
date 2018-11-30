// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Main package for the bsondump tool.
package main

import (
	"github.com/mongodb/mongo-tools/bsondump"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"os"
)

func main() {
	// initialize command-line opts
	opts := options.New("bsondump", bsondump.Usage, options.EnabledOptions{})
	bsonDumpOpts := &bsondump.BSONDumpOptions{}
	opts.AddOptions(bsonDumpOpts)

	args, err := opts.ParseArgs(os.Args[1:])
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'bsondump --help' for more information")
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
		log.Logvf(log.Always, "too many positional arguments: %v", args)
		log.Logvf(log.Always, "try 'bsondump --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	// If the user specified a bson input file
	if len(args) == 1 {
		if bsonDumpOpts.BSONFileName != "" {
			log.Logvf(log.Always, "Cannot specify both a positional argument and --bsonFile")
			os.Exit(util.ExitBadOptions)
		}

		bsonDumpOpts.BSONFileName = args[0]
	}

	dumper := bsondump.BSONDump{
		ToolOptions:     opts,
		BSONDumpOptions: bsonDumpOpts,
	}

	reader, err := bsonDumpOpts.GetBSONReader()
	if err != nil {
		log.Logvf(log.Always, "Getting BSON Reader Failed: %v", err)
		os.Exit(util.ExitError)
	}
	dumper.BSONSource = db.NewBSONSource(reader)
	defer dumper.BSONSource.Close()

	writer, err := bsonDumpOpts.GetWriter()
	if err != nil {
		log.Logvf(log.Always, "Getting Writer Failed: %v", err)
		os.Exit(util.ExitError)
	}
	dumper.Out = writer
	defer dumper.Out.Close()

	log.Logvf(log.DebugLow, "running bsondump with --objcheck: %v", bsonDumpOpts.ObjCheck)

	if len(bsonDumpOpts.Type) != 0 && bsonDumpOpts.Type != "debug" && bsonDumpOpts.Type != "json" {
		log.Logvf(log.Always, "Unsupported output type '%v'. Must be either 'debug' or 'json'", bsonDumpOpts.Type)
		os.Exit(util.ExitBadOptions)
	}

	var numFound int
	if bsonDumpOpts.Type == "debug" {
		numFound, err = dumper.Debug()
	} else {
		numFound, err = dumper.JSON()
	}

	log.Logvf(log.Always, "%v objects found", numFound)
	if err != nil {
		log.Logv(log.Always, err.Error())
		os.Exit(util.ExitError)
	}
}
