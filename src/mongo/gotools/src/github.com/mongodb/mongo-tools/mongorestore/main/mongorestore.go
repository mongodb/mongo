// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Main package for the mongorestore tool.
package main

import (
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/signals"
	"github.com/mongodb/mongo-tools-common/util"
	"github.com/mongodb/mongo-tools/mongorestore"

	"os"
)

var (
	VersionStr = "built-without-version-string"
	GitCommit  = "build-without-git-commit"
)

func main() {
	opts, err := mongorestore.ParseOptions(os.Args[1:], VersionStr, GitCommit)

	if err != nil {
		log.Logv(log.Always, err.Error())
		log.Logvf(log.Always, "try 'mongorestore --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	// print help or version info, if specified
	if opts.PrintHelp(false) {
		return
	}

	if opts.PrintVersion() {
		return
	}

	restore, err := mongorestore.New(opts)
	if err != nil {
		log.Logvf(log.Always, err.Error())
		os.Exit(util.ExitError)
	}
	defer restore.Close()

	finishedChan := signals.HandleWithInterrupt(restore.HandleInterrupt)
	defer close(finishedChan)

	if err = restore.Restore(); err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		if err == util.ErrTerminated {
			os.Exit(util.ExitKill)
		}
		os.Exit(util.ExitError)
	}
}
