package main

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongoimport"
	"github.com/shelman/mongo-tools-proto/mongoimport/options"
	"os"
)

func main() {
	// initialize command-line opts
	usageStr := " --host myhost --db my_cms --collection docs < mydocfile." +
		"json \n\nImport CSV, TSV or JSON data into MongoDB.\n\nWhen importing " +
		"JSON documents, each document must be a separate line of the input file."
	opts := commonopts.New("mongoimport", "0.0.1", usageStr)

	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	ingestOpts := &options.IngestOptions{}
	opts.AddOptions(ingestOpts)

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
	sessionProvider, err := db.InitSessionProvider(*opts)
	if err != nil {
		util.Panicf("error initializing database session: %v", err)
	}

	importer := mongoimport.MongoImport{
		ToolOptions:     opts,
		InputOptions:    inputOpts,
		IngestOptions:   ingestOpts,
		SessionProvider: sessionProvider,
	}

	if err = importer.ValidateSettings(); err != nil {
		util.PrintfTimeStamped("Error validating settings: %v\n", err)
		os.Exit(1)
	}

	numDocs, err := importer.ImportDocuments()
	if !opts.Quiet {
		message := fmt.Sprintf("imported 1 object\n")
		if numDocs != 1 {
			message = fmt.Sprintf("imported %v objects\n", numDocs)
		}
		util.PrintfTimeStamped(message)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error importing documents: %v\n", err)
		os.Exit(1)
	}
}
