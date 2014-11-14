package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongoexport"
	"github.com/mongodb/mongo-tools/mongoexport/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongoexport", "<options>", commonopts.EnabledOptions{Auth: true, Connection: true, Namespace: true})

	outputOpts := &options.OutputFormatOptions{}
	opts.AddOptions(outputOpts)
	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v", err)
		os.Exit(1)
	}
	if len(args) != 0 {
		log.Logf(log.Always, "error parsing command line: too many positional options: %v", args)
		os.Exit(1)
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

	exporter := mongoexport.MongoExport{
		ToolOptions:     *opts,
		OutputOpts:      outputOpts,
		InputOpts:       inputOpts,
		SessionProvider: db.NewSessionProvider(*opts),
	}

	err = exporter.ValidateSettings()
	if err != nil {
		log.Logf(log.Always, "error validating settings: %v", err)
		os.Exit(1)
	}

	numDocs, err := exporter.Export()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	if !opts.Quiet {
		if numDocs == 1 {
			log.Logf(log.Always, "exported %v record", numDocs)
		} else {
			log.Logf(log.Always, "exported %v records", numDocs)
		}
	}
}
