package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongorestore"
	"github.com/mongodb/mongo-tools/mongorestore/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongorestore", "<options>")
	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &options.OutputOptions{}
	opts.AddOptions(outputOpts)

	args, err := opts.Parse()
	if err != nil {
		fmt.Printf("error parsing command line options: %v\n\n", err)
		fmt.Printf("try 'mongorestore --help' for more information\n")
		os.Exit(2)
	}

	// print help or version info, if specified
	if opts.PrintHelp() {
		return
	}
	if opts.PrintVersion() {
		return
	}

	log.SetVerbosity(opts.Verbosity)

	targetDir := ""
	if inputOpts.Directory != "" {
		targetDir = inputOpts.Directory
		log.Log(0, "using --dir flag instead of arguments")
	} else {
		if len(args) == 0 {
			targetDir = "dump"
		} else {
			targetDir = args[0]
			if len(args) > 1 {
				fmt.Printf("error parsing command line: too many arguments\n")
				util.ExitFail()
				return
			}
		}
	}

	targetDir = util.ToUniversalPath(targetDir)

	opts.Direct = true

	restore := mongorestore.MongoRestore{
		ToolOptions:     opts,
		OutputOptions:   outputOpts,
		InputOptions:    inputOpts,
		TargetDirectory: targetDir,
	}

	err = restore.Init()
	if err != nil {
		util.Exitf(1, "%v", err)
	}

	err = restore.Restore()
	if err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		util.ExitFail()
		return
	}

}
