package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
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

func printHelpAndExit() {
	fmt.Println("try 'mongofiles --help' for more information")
	os.Exit(1)
}

func main() {

	// initialize command-line opts
	opts := commonopts.New("mongofiles", "0.0.1", Usage)

	storageOpts := &options.StorageOptions{}
	opts.AddOptions(storageOpts)

	args, err := opts.Parse()
	if err != nil {
		fmt.Printf("Error parsing command line options: %v\n", err)
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

	fileName, err := mongofiles.ValidateCommand(args)
	if err != nil {
		fmt.Printf("Error: %v\n", err)
		printHelpAndExit()
	}

	// initialize logger
	log.InitToolLogger(opts.Verbosity)

	mongofiles := mongofiles.MongoFiles{
		ToolOptions:    opts,
		StorageOptions: storageOpts,
		Command:        args[0],
		FileName:       fileName,
	}

	err = mongofiles.Init()
	if err != nil {
		fmt.Printf("Error initializing mongofiles: %v\n", err)
		os.Exit(1)
	}

	output, err := mongofiles.Run(true)
	if err != nil {
		fmt.Printf("%v\n", err)
		os.Exit(1)
	}
	fmt.Printf("%s", output)
}
