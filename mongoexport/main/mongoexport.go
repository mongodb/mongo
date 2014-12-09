package main

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoexport"
	"os"
)

func main() {
	// initialize command-line opts
	opts := options.New("mongoexport", "<options>", options.EnabledOptions{Auth: true, Connection: true, Namespace: true})

	outputOpts := &mongoexport.OutputFormatOptions{}
	opts.AddOptions(outputOpts)
	inputOpts := &mongoexport.InputOptions{}
	opts.AddOptions(inputOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v", err)
		opts.PrintHelp(true)
		os.Exit(util.ExitBadOptions)
	}
	if len(args) != 0 {
		log.Logf(log.Always, "error parsing command line: too many positional options: %v", args)
		os.Exit(util.ExitBadOptions)
	}

	log.SetVerbosity(opts.Verbosity)

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")

	provider, err := db.NewSessionProvider(*opts)
	if err != nil {
		log.Logf(log.Always, "error connecting to host: %v\n", err)
		os.Exit(util.ExitError)
	}
	exporter := mongoexport.MongoExport{
		ToolOptions:     *opts,
		OutputOpts:      outputOpts,
		InputOpts:       inputOpts,
		SessionProvider: provider,
	}

	err = exporter.ValidateSettings()
	if err != nil {
		log.Logf(log.Always, "error validating settings: %v", err)
		opts.PrintHelp(true)
		os.Exit(util.ExitError)
	}

	numDocs, err := exporter.Export()
	if err != nil {
		log.Logf(log.Always, "Failed: %v\n", err)
		os.Exit(util.ExitError)
	}

	if numDocs == 1 {
		log.Logf(log.Always, "exported %v record", numDocs)
	} else {
		log.Logf(log.Always, "exported %v records", numDocs)
	}
}
