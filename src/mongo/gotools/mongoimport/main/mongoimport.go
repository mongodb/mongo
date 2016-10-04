// Main package for the mongoimport tool.
package main

import (
	"fmt"
	"os"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoimport"
)

func main() {
	// initialize command-line opts
	opts := options.New("mongoimport", mongoimport.Usage,
		options.EnabledOptions{Auth: true, Connection: true, Namespace: true})

	inputOpts := &mongoimport.InputOptions{}
	opts.AddOptions(inputOpts)
	ingestOpts := &mongoimport.IngestOptions{}
	opts.AddOptions(ingestOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'mongoimport --help' for more information")
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

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")
	opts.ReplicaSetName = setName

	// create a session provider to connect to the db
	sessionProvider, err := db.NewSessionProvider(*opts)
	defer sessionProvider.Close()
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}
	sessionProvider.SetBypassDocumentValidation(ingestOpts.BypassDocumentValidation)

	m := mongoimport.MongoImport{
		ToolOptions:     opts,
		InputOptions:    inputOpts,
		IngestOptions:   ingestOpts,
		SessionProvider: sessionProvider,
	}

	if err = m.ValidateSettings(args); err != nil {
		log.Logvf(log.Always, "error validating settings: %v", err)
		log.Logvf(log.Always, "try 'mongoimport --help' for more information")
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
