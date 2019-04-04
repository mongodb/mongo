// Copyright (C) MerizoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Main package for the merizoimport tool.
package main

import (
	"fmt"
	"os"

	"github.com/merizodb/merizo-tools/common/db"
	"github.com/merizodb/merizo-tools/common/log"
	"github.com/merizodb/merizo-tools/common/options"
	"github.com/merizodb/merizo-tools/common/signals"
	"github.com/merizodb/merizo-tools/common/util"
	"github.com/merizodb/merizo-tools/merizoimport"
)

func main() {
	// initialize command-line opts
	opts := options.New("merizoimport", merizoimport.Usage,
		options.EnabledOptions{Auth: true, Connection: true, Namespace: true, URI: true})

	inputOpts := &merizoimport.InputOptions{}
	opts.AddOptions(inputOpts)
	ingestOpts := &merizoimport.IngestOptions{}
	opts.AddOptions(ingestOpts)
	opts.URI.AddKnownURIParameters(options.KnownURIOptionsWriteConcern)

	args, err := opts.ParseArgs(os.Args[1:])
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'merizoimport --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	log.SetVerbosity(opts.Verbosity)
	signals.Handle()

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// verify uri options and log them
	opts.URI.LogUnsupportedOptions()

	// create a session provider to connect to the db
	sessionProvider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}
	defer sessionProvider.Close()
	sessionProvider.SetBypassDocumentValidation(ingestOpts.BypassDocumentValidation)

	m := merizoimport.MongoImport{
		ToolOptions:     opts,
		InputOptions:    inputOpts,
		IngestOptions:   ingestOpts,
		SessionProvider: sessionProvider,
	}

	if err = m.ValidateSettings(args); err != nil {
		log.Logvf(log.Always, "error validating settings: %v", err)
		log.Logvf(log.Always, "try 'merizoimport --help' for more information")
		os.Exit(util.ExitError)
	}

	numDocs, err := m.ImportDocuments()
	if !opts.Quiet {
		if err != nil {
			log.Logvf(log.Always, "Failed: %v", err)
		}
		message := fmt.Sprintf("imported 1 document")
		if numDocs != 1 {
			message = fmt.Sprintf("imported %v documents", numDocs)
		}
		log.Logvf(log.Always, message)
	}
	if err != nil {
		os.Exit(util.ExitError)
	}
}
