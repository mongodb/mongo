// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Main package for the mongotop tool.
package main

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongotop"
	"gopkg.in/mgo.v2"
	"os"
	"strconv"
	"time"
)

func main() {
	// initialize command-line opts
	opts := options.New("mongotop", mongotop.Usage,
		options.EnabledOptions{Auth: true, Connection: true, Namespace: false, URI: true})
	opts.UseReadOnlyHostDescription()

	// add mongotop-specific options
	outputOpts := &mongotop.Output{}
	opts.AddOptions(outputOpts)

	args, err := opts.ParseArgs(os.Args[1:])
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'mongotop --help' for more information")
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

	// verify uri options and log them
	opts.URI.LogUnsupportedOptions()

	if len(args) > 1 {
		log.Logvf(log.Always, "too many positional arguments")
		log.Logvf(log.Always, "try 'mongotop --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	sleeptime := 1 // default to 1 second sleep time
	if len(args) > 0 {
		sleeptime, err = strconv.Atoi(args[0])
		if err != nil || sleeptime <= 0 {
			log.Logvf(log.Always, "invalid sleep time: %v", args[0])
			os.Exit(util.ExitBadOptions)
		}
	}
	if outputOpts.RowCount < 0 {
		log.Logvf(log.Always, "invalid value for --rowcount: %v", outputOpts.RowCount)
		os.Exit(util.ExitBadOptions)
	}

	if opts.Auth.Username != "" && opts.Auth.Source == "" && !opts.Auth.RequiresExternalDB() {
		if opts.URI != nil && opts.URI.ConnectionString != "" {
			log.Logvf(log.Always, "authSource is required when authenticating against a non $external database")
			os.Exit(util.ExitBadOptions)
		}
		log.Logvf(log.Always, "--authenticationDatabase is required when authenticating against a non $external database")
		os.Exit(util.ExitBadOptions)
	}

	// create a session provider to connect to the db
	sessionProvider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}

	if opts.ReplicaSetName == "" {
		sessionProvider.SetReadPreference(mgo.PrimaryPreferred)
	}

	// fail fast if connecting to a mongos
	isMongos, err := sessionProvider.IsMongos()
	if err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
	if isMongos {
		log.Logvf(log.Always, "cannot run mongotop against a mongos")
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
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
}
