package main

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongoexport"
	"github.com/shelman/mongo-tools-proto/mongoexport/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongoexport", "0.0.1", "<options> <sleeptime>")

	outputOpts := &options.OutputFormatOptions{}
	opts.AddOptions(outputOpts)
	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)

	_, err := opts.Parse()
	if err != nil {
		util.Panicf("error parsing command line options: %v", err)
	}

	// print help, if specified
	if opts.PrintHelp() {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// create a session provider to connect to the db
	sessionProvider, err := db.InitSessionProvider(opts)
	if err != nil {
		util.Panicf("error initializing database session: %v", err)
	}

	exporter := mongoexport.MongoExport{
		ToolOptions:     opts,
		OutputOpts:      outputOpts,
		InputOpts:       inputOpts,
		SessionProvider: sessionProvider,
	}

	err = exporter.ValidateSettings()
	if err != nil {
		//TODO log to stderr for real
		fmt.Printf("Error: %v\n", err)
		os.Exit(1)
	}

	numDocs, err := exporter.Export()
	if err != nil {
		//TODO log to stderr for real
		fmt.Printf("Error: %v\n", err)
		os.Exit(1)
	}

	if !opts.Quiet {
		if numDocs == 1 {
			fmt.Fprintf(os.Stderr, fmt.Sprintf("exported %v record\n", numDocs))
		} else {
			fmt.Fprintf(os.Stderr, fmt.Sprintf("exported %v records\n", numDocs))
		}
	}
}
