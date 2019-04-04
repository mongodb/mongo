// Copyright (C) MerizoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Main package for the merizofiles tool.
package main

import (
	"github.com/merizodb/merizo-tools/common/db"
	"github.com/merizodb/merizo-tools/common/log"
	"github.com/merizodb/merizo-tools/common/options"
	"github.com/merizodb/merizo-tools/common/signals"
	"github.com/merizodb/merizo-tools/common/util"
	"github.com/merizodb/merizo-tools/merizofiles"

	"fmt"
	"os"
)

func main() {
	// initialize command-line opts
	opts := options.New("merizofiles", merizofiles.Usage, options.EnabledOptions{Auth: true, Connection: true, Namespace: false, URI: true})

	storageOpts := &merizofiles.StorageOptions{}
	opts.AddOptions(storageOpts)
	inputOpts := &merizofiles.InputOptions{}
	opts.AddOptions(inputOpts)
	opts.URI.AddKnownURIParameters(options.KnownURIOptionsReadPreference)

	args, err := opts.ParseArgs(os.Args[1:])
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'merizofiles --help' for more information")
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

	// add the specified database to the namespace options struct
	opts.Namespace.DB = storageOpts.DB

	// create a session provider to connect to the db
	provider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}
	defer provider.Close()
	mf := merizofiles.MerizoFiles{
		ToolOptions:     opts,
		StorageOptions:  storageOpts,
		SessionProvider: provider,
		InputOptions:    inputOpts,
	}

	if err := mf.ValidateCommand(args); err != nil {
		log.Logvf(log.Always, "%v", err)
		log.Logvf(log.Always, "try 'merizofiles --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	output, err := mf.Run(true)
	if err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
	fmt.Printf("%s", output)
}
