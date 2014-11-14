package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/bsondump"
	"github.com/mongodb/mongo-tools/bsondump/options"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("bsondump", "<file>", commonopts.EnabledOptions{})
	bsonDumpOpts := &options.BSONDumpOptions{}
	opts.AddOptions(bsonDumpOpts)

	extra, err := opts.Parse()
	if err != nil {
		log.Logf(log.Always, "error parsing command line options: %v", err)
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

	// pull out the filename
	filename := ""
	if len(extra) == 0 {
		opts.PrintHelp(true)
		return
	} else if len(extra) > 1 {
		log.Log(log.Always, "Too many positional operators.")
		opts.PrintHelp(true)
		os.Exit(1)
	} else {
		filename = extra[0]
		if filename == "" {
			log.Log(log.Always, "Filename must not be blank.")
			opts.PrintHelp(true)
			os.Exit(1)
		}
	}

	dumper := bsondump.BSONDump{
		ToolOptions:     opts,
		BSONDumpOptions: bsonDumpOpts,
		FileName:        filename,
		Out:             os.Stdout,
	}

	if bsonDumpOpts.Type == "debug" {
		err = dumper.Debug()
	} else if bsonDumpOpts.Type == "json" || bsonDumpOpts.Type == "" {
		err = dumper.Dump()
	} else {
		err = fmt.Errorf("Unsupported output type '%v'. Must be either 'debug' or 'json'", bsonDumpOpts.Type)
	}
	if err != nil {
		log.Log(log.Always, err.Error())
		os.Exit(1)
	}
}
