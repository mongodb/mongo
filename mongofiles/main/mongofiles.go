package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongofiles"
	"github.com/mongodb/mongo-tools/mongofiles/options"
	"os"
)

const (
	Usage = `[options] command [gridfs filename]
        command:
          one of (list|search|put|get|delete)
          list - list all files.  'gridfs filename' is an optional prefix
                 which listed filenames must begin with.
          search - search all files. 'gridfs filename' is a substring
                   which listed filenames must contain.
          put - add a file with filename 'gridfs filename'
          get - get a file with filename 'gridfs filename'
          delete - delete all files with filename 'gridfs filename'
        `
)

func main() {

	// initialize command-line opts
	opts := commonOpts.New("mongofiles", Usage)

	storageOpts := &options.StorageOptions{}
	opts.AddOptions(storageOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v", err)
		opts.PrintHelp(true)
		os.Exit(1)
	}

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	log.SetVerbosity(opts.Verbosity)

	fileName, err := mongofiles.ValidateCommand(args)
	if err != nil {
		log.Logf(log.Always, "error: %v", err)
		opts.PrintHelp(true)
		os.Exit(1)
	}

	// create a session provider to connect to the db
	sessionProvider := db.NewSessionProvider(*opts)

	mongofiles := mongofiles.MongoFiles{
		ToolOptions:     opts,
		StorageOptions:  storageOpts,
		SessionProvider: sessionProvider,
		Command:         args[0],
		FileName:        fileName,
	}

	output, err := mongofiles.Run(true)
	if err != nil {
		log.Logf(log.Always, "%v", err)
		os.Exit(1)
	}
	fmt.Printf("%s", output)
}
