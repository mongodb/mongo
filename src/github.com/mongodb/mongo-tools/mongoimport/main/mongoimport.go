package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoimport"
	"github.com/mongodb/mongo-tools/mongoimport/options"
	"os"
)

func main() {
	// initialize command-line opts
	usageStr := " --host myhost --db my_cms --collection docs < mydocfile." +
		"json \n\nImport CSV, TSV or JSON data into MongoDB.\n\nWhen importing " +
		"JSON documents, each document must be a separate line of the input file."
	opts := commonopts.New("mongoimport", usageStr)

	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	ingestOpts := &options.IngestOptions{}
	opts.AddOptions(ingestOpts)

	args, err := opts.Parse()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error parsing command line options: %v\n", err)
		util.ExitFail()
	}

	// print help, if specified
	if opts.PrintHelp() {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	opts.Direct = true

	// create a session provider to connect to the db
	sessionProvider := db.NewSessionProvider(*opts)

	importer := mongoimport.MongoImport{
		ToolOptions:     opts,
		InputOptions:    inputOpts,
		IngestOptions:   ingestOpts,
		SessionProvider: sessionProvider,
	}

	if err = importer.ValidateSettings(args); err != nil {
		util.PrintfTimeStamped("Error validating settings: %v\n", err)
		util.ExitFail()
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
		util.ExitFail()
	}
}
