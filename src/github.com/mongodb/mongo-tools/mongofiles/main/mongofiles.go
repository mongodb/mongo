package main

import (
	"fmt"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongofiles"
	"github.com/mongodb/mongo-tools/mongofiles/options"
	"github.com/mongodb/mongo-tools/common/log"
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

func printHelpAndExit() {
	fmt.Println("try 'mongofiles --help' for more information")
	os.Exit(1)
}

func main() {

	// initialize command-line opts
	opts := commonOpts.New("mongofiles", Usage)

	storageOpts := &options.StorageOptions{}
	opts.AddOptions(storageOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logf(0, "error parsing command line options: %v", err)
		printHelpAndExit()
	}

	// print help, if specified
	if opts.PrintHelp() {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	log.SetVerbosity(opts.Verbosity)

	fileName, err := mongofiles.ValidateCommand(args)
	if err != nil {
		log.Logf(0, "Error: %v", err)
		printHelpAndExit()
	}

	mongofiles := mongofiles.MongoFiles{
		ToolOptions:    opts,
		StorageOptions: storageOpts,
		Command:        args[0],
		FileName:       fileName,
	}

	err = mongofiles.Init()
	if err != nil {
		log.Logf(0, "Error initializing mongofiles: %v", err)
		os.Exit(1)
	}

	output, err := mongofiles.Run(true)
	if err != nil {
		log.Logf(0, "%v", err)
		os.Exit(1)
	}
	fmt.Printf("%s", output)
}
