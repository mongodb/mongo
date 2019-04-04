// Copyright (C) MerizoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package main

import (
	"github.com/jessevdk/go-flags"
	"github.com/merizodb/merizo-tools/common/options"
	"github.com/merizodb/merizo-tools/merizoreplay"

	"fmt"
	"os"
	"runtime"
)

const (
	ExitOk       = 0
	ExitError    = 1
	ExitNonFatal = 3
	// Go reserves exit code 2 for its own use
)

func main() {
	versionOpts := merizoreplay.VersionOptions{}
	versionFlagParser := flags.NewParser(&versionOpts, flags.Default)
	versionFlagParser.Options = flags.IgnoreUnknown
	_, err := versionFlagParser.Parse()
	if err != nil {
		os.Exit(ExitError)
	}

	if versionOpts.PrintVersion() {
		os.Exit(ExitOk)
	}

	if runtime.NumCPU() == 1 {
		fmt.Fprint(os.Stderr, "merizoreplay must be run with multiple threads")
		os.Exit(ExitError)
	}

	opts := merizoreplay.Options{}

	var parser = flags.NewParser(&opts, flags.Default)

	playCmd := &merizoreplay.PlayCommand{GlobalOpts: &opts}
	playCmdParser, err := parser.AddCommand("play", "Play captured traffic against a merizodb instance", "", playCmd)
	if err != nil {
		panic(err)
	}
	if options.BuiltWithSSL {
		playCmd.SSLOpts = &options.SSL{}
		_, err := playCmdParser.AddGroup("ssl", "", playCmd.SSLOpts)
		if err != nil {
			panic(err)
		}
	}

	_, err = parser.AddCommand("record", "Convert network traffic into merizodb queries", "",
		&merizoreplay.RecordCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.AddCommand("monitor", "Inspect live or pre-recorded merizodb traffic", "",
		&merizoreplay.MonitorCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.AddCommand("filter", "Filter playback file", "",
		&merizoreplay.FilterCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.Parse()

	if err != nil {
		switch err.(type) {
		case merizoreplay.ErrPacketsDropped:
			os.Exit(ExitNonFatal)
		default:
			os.Exit(ExitError)
		}
	}
}
