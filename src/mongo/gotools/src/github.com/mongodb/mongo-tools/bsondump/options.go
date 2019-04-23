// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package bsondump

import (
	"fmt"

	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
)

var Usage = `<options> <file>

View and debug .bson files.

See http://docs.mongodb.org/manual/reference/program/bsondump/ for more information.`

// Options contains all the possible options used to configure bsondump.
type Options struct {
	*options.ToolOptions
	*OutputOptions
}

// Types out output supported by the --type option
const (
	DebugOutputType = "debug"
	JSONOutputType  = "json"
)

type OutputOptions struct {
	// Format to display the BSON data file
	Type string `long:"type" value-name:"<type>" default:"json" default-mask:"-" description:"type of output: debug, json (default 'json')"`

	// Validate each BSON document before displaying
	ObjCheck bool `long:"objcheck" description:"validate BSON during processing"`

	// Display JSON data with indents
	Pretty bool `long:"pretty" description:"output JSON formatted to be human-readable"`

	// Path to input BSON file
	BSONFileName string `long:"bsonFile" description:"path to BSON file to dump to JSON; default is stdin"`

	// Path to output file
	OutFileName string `long:"outFile" description:"path to output file to dump BSON to; default is stdout"`
}

func (*OutputOptions) Name() string {
	return "output"
}

// ParseOptions translates the command line arguments into an Options used to configure BSONDump.
func ParseOptions(rawArgs []string, versionStr, gitCommit string) (Options, error) {
	toolOpts := options.New("bsondump", versionStr, gitCommit, Usage, options.EnabledOptions{})
	outputOpts := &OutputOptions{}
	toolOpts.AddOptions(outputOpts)

	args, err := toolOpts.ParseArgs(rawArgs)
	if err != nil {
		return Options{}, fmt.Errorf("error parsing command line options: %v", err)
	}

	log.SetVerbosity(toolOpts.Verbosity)

	if len(args) > 1 {
		return Options{}, fmt.Errorf("too many positional arguments: %v", args)
	}

	// If the user specified a bson input file
	if len(args) == 1 {
		if outputOpts.BSONFileName != "" {
			return Options{}, fmt.Errorf("cannot specify both a positional argument and --bsonFile")
		}

		outputOpts.BSONFileName = args[0]
	}

	switch outputOpts.Type {
	case "", DebugOutputType, JSONOutputType:
		return Options{toolOpts, outputOpts}, nil
	default:
		return Options{}, fmt.Errorf("unsupported output type '%v'. Must be either '%v' or '%v'", DebugOutputType, JSONOutputType, outputOpts.Type)
	}
}
