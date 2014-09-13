package main

import (
	"fmt"
	//"github.com/shelman/mongo-tools-proto/common/db"
	"github.com/shelman/mongo-tools-proto/bsondump"
	"github.com/shelman/mongo-tools-proto/bsondump/options"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"os"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("bsondump", "0.0.1", "<file>")
	bsonDumpOpts := &options.BSONDumpOptions{}
	opts.AddOptions(bsonDumpOpts)

	extra, err := opts.Parse()
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

	// pull out the filename
	filename := ""
	if len(extra) == 0 {
		opts.PrintHelp()
		return
	} else if len(extra) > 1 {
		fmt.Println("Too many positional operators.")
		opts.PrintHelp()
		os.Exit(1)
		return
	} else {
		filename = extra[0]
		if filename == "" {
			fmt.Println("Filename must not be blank.")
			opts.PrintHelp()
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
		err = fmt.Errorf("Unsupported output type '%'. Must be either 'debug' or 'json'", bsonDumpOpts.Type)
	}
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
}
