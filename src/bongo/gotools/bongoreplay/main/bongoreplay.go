package main

import (
	"github.com/jessevdk/go-flags"
	"github.com/bongodb/bongo-tools/bongoreplay"

	"os"
)

const (
	ExitOk       = 0
	ExitError    = 1
	ExitNonFatal = 3
	// Go reserves exit code 2 for its own use
)

func main() {
	versionOpts := bongoreplay.VersionOptions{}
	versionFlagParser := flags.NewParser(&versionOpts, flags.Default)
	versionFlagParser.Options = flags.IgnoreUnknown
	_, err := versionFlagParser.Parse()
	if err != nil {
		os.Exit(ExitError)
	}

	if versionOpts.PrintVersion() {
		os.Exit(ExitOk)
	}

	opts := bongoreplay.Options{}

	var parser = flags.NewParser(&opts, flags.Default)

	_, err = parser.AddCommand("play", "Play captured traffic against a bongodb instance", "",
		&bongoreplay.PlayCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.AddCommand("record", "Convert network traffic into bongodb queries", "",
		&bongoreplay.RecordCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.AddCommand("monitor", "Inspect live or pre-recorded bongodb traffic", "",
		&bongoreplay.MonitorCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.Parse()

	if err != nil {
		switch err.(type) {
		case bongoreplay.ErrPacketsDropped:
			os.Exit(ExitNonFatal)
		default:
			os.Exit(ExitError)
		}
	}
}
