package main

import (
	"fmt"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongostat"
	"github.com/mongodb/mongo-tools/mongostat/options"
	"os"
	"runtime"
	"strconv"
	"time"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongostat", "0.0.1", "<options>")

	// add mongotop-specific options
	statOpts := &options.StatOptions{}
	opts.AddOptions(statOpts)

	extra, err := opts.Parse()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Invalid options: %v\n", err)
		opts.PrintHelp()
		os.Exit(1)
	}

	sleepInterval := 1
	if len(extra) > 0 {
		if len(extra) != 1 {
			fmt.Fprintf(os.Stderr, "Too many positional operators\n")
			opts.PrintHelp()
			os.Exit(1)
		}
		sleepInterval, err = strconv.Atoi(extra[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "Bad sleep interval: %v\n", extra[0])
			os.Exit(1)
		}
		if sleepInterval < 1 {
			fmt.Fprintf(os.Stderr, "Sleep interval must be at least 1 second\n")
			os.Exit(1)
		}
	}

	// print help, if specified
	if opts.PrintHelp() {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	var discoverChan chan string
	if statOpts.Discover {
		discoverChan = make(chan string, 128)
	}

	opts.Direct = true
	stat := &mongostat.MongoStat{
		Options:       opts,
		StatOptions:   statOpts,
		Nodes:         map[string]*mongostat.NodeMonitor{},
		Discovered:    discoverChan,
		SleepInterval: time.Duration(sleepInterval) * time.Second,
		Cluster: &mongostat.ClusterMonitor{
			ReportChan:    make(chan mongostat.StatLine),
			LastStatLines: map[string]mongostat.StatLine{},
			NoHeaders:     statOpts.NoHeaders,
		},
	}

	seedHost := opts.Host
	if opts.Port != "" {
		seedHost = fmt.Sprintf("%s:%s", opts.Host, opts.Port)
	}

	stat.AddNewNode(seedHost)

	// kick it off
	err = stat.Run()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v", err)

		//Required for stat1.js: exit code on failure is -1, or 255 for Windows
		failureCode := -1
		if runtime.GOOS == "windows" {
			failureCode = 255
		}
		os.Exit(failureCode)
	}

}
