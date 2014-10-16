package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongodump"
	"github.com/mongodb/mongo-tools/mongodump/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongodump", "<options>")

	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &options.OutputOptions{}
	opts.AddOptions(outputOpts)

	_, err := opts.Parse()
	if err != nil {
		fmt.Printf("error parsing command line options: %v\n\n", err)
		fmt.Printf("try 'mongodump --help' for more information\n")
		os.Exit(2)
	}

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// init logger
	log.SetVerbosity(opts.Verbosity)

	dump := mongodump.MongoDump{
		ToolOptions:   opts,
		OutputOptions: outputOpts,
		InputOptions:  inputOpts,
	}

	err = dump.Init()
	if err != nil {
		util.Exitf(1, "%v", err)
	}

	err = dump.Dump()
	if err != nil {
		util.Exitf(1, "%v", err)
	}

}
