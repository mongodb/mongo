package main

import (
	"fmt"
	//"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoexport"
	"github.com/mongodb/mongo-tools/mongoexport/options"
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

	exporter := mongoexport.MongoExport{
		ToolOptions: *opts,
		OutputOpts:  outputOpts,
		InputOpts:   inputOpts,
	}

	err = exporter.ValidateSettings()
	if err != nil {
		//TODO log to stderr for real
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
	}

	numDocs, err := exporter.Export()
	if err != nil {
		//TODO log to stderr for real
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
