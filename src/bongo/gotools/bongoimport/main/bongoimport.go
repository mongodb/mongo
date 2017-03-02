// Main package for the bongoimport tool.
package main

import (
	"fmt"
	"os"

	"github.com/bongodb/bongo-tools/common/db"
	"github.com/bongodb/bongo-tools/common/log"
	"github.com/bongodb/bongo-tools/common/options"
	"github.com/bongodb/bongo-tools/common/signals"
	"github.com/bongodb/bongo-tools/common/util"
	"github.com/bongodb/bongo-tools/bongoimport"
)

func main() {
	// initialize command-line opts
	opts := options.New("bongoimport", bongoimport.Usage,
		options.EnabledOptions{Auth: true, Connection: true, Namespace: true})

	inputOpts := &bongoimport.InputOptions{}
	opts.AddOptions(inputOpts)
	ingestOpts := &bongoimport.IngestOptions{}
	opts.AddOptions(ingestOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'bongoimport --help' for more information")
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
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}
	defer sessionProvider.Close()
	sessionProvider.SetBypassDocumentValidation(ingestOpts.BypassDocumentValidation)

	m := bongoimport.BongoImport{
		ToolOptions:     opts,
		InputOptions:    inputOpts,
		IngestOptions:   ingestOpts,
		SessionProvider: sessionProvider,
	}

	if err = m.ValidateSettings(args); err != nil {
		log.Logvf(log.Always, "error validating settings: %v", err)
		log.Logvf(log.Always, "try 'bongoimport --help' for more information")
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
