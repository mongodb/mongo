package main

import (
	"github.com/jessevdk/go-flags"
	"github.com/mongodb/mongo-tools/mongoreplay"

	"os"
)

const (
	ExitOk       = 0
	ExitError    = 1
	ExitNonFatal = 3
	// Go reserves exit code 2 for its own use
)

func main() {
	opts := mongoreplay.Options{}

	var parser = flags.NewParser(&opts, flags.Default)

	_, err := parser.AddCommand("play", "Play captured traffic against a mongodb instance", "",
		&mongoreplay.PlayCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.AddCommand("record", "Convert network traffic into mongodb queries", "",
		&mongoreplay.RecordCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	_, err = parser.AddCommand("monitor", "Inspect live or pre-recorded mongodb traffic", "",
		&mongoreplay.MonitorCommand{GlobalOpts: &opts})
	if err != nil {
		panic(err)
	}

	parser.Options = flags.IgnoreUnknown
	parser.Parse()
	if opts.PrintVersion() {
		os.Exit(ExitOk)
	}

	parser.Options = flags.Default
	_, err = parser.Parse()

	if err != nil {
		switch err.(type) {
		case mongoreplay.ErrPacketsDropped:
			os.Exit(ExitNonFatal)
		default:
			os.Exit(ExitError)
		}
	}
}
