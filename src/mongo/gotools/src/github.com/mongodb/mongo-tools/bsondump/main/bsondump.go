// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Main package for the bsondump tool.
package main

import (
	"os"

	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/signals"
	"github.com/mongodb/mongo-tools-common/util"
	"github.com/mongodb/mongo-tools/bsondump"
)

var (
	VersionStr = "built-without-version-string"
	GitCommit  = "build-without-git-commit"
)

func main() {
	// initialize command-line opts
	opts, err := bsondump.ParseOptions(os.Args[1:], VersionStr, GitCommit)
	if err != nil {
		log.Logvf(log.Always, "%v", err)
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

	signals.Handle()

	dumper, err := bsondump.New(opts)
	if err != nil {
		log.Logv(log.Always, err.Error())
		os.Exit(util.ExitError)
	}
	defer func() {
		err := dumper.Close()
		if err != nil {
			log.Logvf(log.Always, "error cleaning up: %v", err)
			os.Exit(util.ExitError)
		}
	}()

	log.Logvf(log.DebugLow, "running bsondump with --objcheck: %v", opts.ObjCheck)

	var numFound int
	if opts.Type == bsondump.DebugOutputType {
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
