package main

import (
	"fmt"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongoexport"
	"github.com/mongodb/mongo-tools/mongoexport/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongoexport", "<options> <sleeptime>")

	outputOpts := &options.OutputFormatOptions{}
	opts.AddOptions(outputOpts)
	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)

	args, err := opts.Parse()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error parsing command line options: %v\n", err)
		os.Exit(1)
	}
	if len(args) != 0 {
		fmt.Fprintf(os.Stderr, "Error parsing command line: too many positional options: %v\n", args)
		os.Exit(1)
	}

	// print help, if specified
	if opts.PrintHelp() {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	exporter := mongoexport.MongoExport{
		ToolOptions: *opts,
		OutputOpts:  outputOpts,
		InputOpts:   inputOpts,
	}

	err = exporter.ValidateSettings()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	err = exporter.Init()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	numDocs, err := exporter.Export()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
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
