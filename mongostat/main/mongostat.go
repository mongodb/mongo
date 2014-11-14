package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongostat"
	"github.com/mongodb/mongo-tools/mongostat/options"
	"os"
	"strconv"
	"time"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New(
		"mongostat",
		"[options] <polling interval in seconds>",
		commonopts.EnabledOptions{Connection: true, Auth: true, Namespace: false})

	// add mongotop-specific options
	statOpts := &options.StatOptions{}
	opts.AddOptions(statOpts)

	extra, err := opts.Parse()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Invalid options: %v\n", err)
		opts.PrintHelp(true)
		util.ExitFail()
		return
	}

	sleepInterval := 1
	if len(extra) > 0 {
		if len(extra) != 1 {
			fmt.Fprintf(os.Stderr, "Too many positional operators\n")
			opts.PrintHelp(true)
			util.ExitFail()
			return
		}
		sleepInterval, err = strconv.Atoi(extra[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "Bad sleep interval: %v\n", extra[0])
			util.ExitFail()
			return
		}
		if sleepInterval < 1 {
			fmt.Fprintf(os.Stderr, "Sleep interval must be at least 1 second\n")
			util.ExitFail()
			return
		}
	}

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	var formatter mongostat.LineFormatter
	formatter = &mongostat.GridLineFormatter{!statOpts.NoHeaders, 10}
	if statOpts.Json {
		formatter = &mongostat.JSONLineFormatter{}
	}

	var discoverChan chan string
	var cluster mongostat.ClusterMonitor
	if statOpts.Discover {
		discoverChan = make(chan string, 128)
		cluster = &mongostat.AsyncClusterMonitor{
			ReportChan:    make(chan mongostat.StatLine),
			LastStatLines: map[string]*mongostat.StatLine{},
			Formatter:     formatter,
		}
	} else {
		cluster = &mongostat.SyncClusterMonitor{
			ReportChan: make(chan mongostat.StatLine),
			Formatter:  formatter,
		}
	}

	opts.Direct = true
	stat := &mongostat.MongoStat{
		Options:       opts,
		StatOptions:   statOpts,
		Nodes:         map[string]*mongostat.NodeMonitor{},
		Discovered:    discoverChan,
		SleepInterval: time.Duration(sleepInterval) * time.Second,
		Cluster:       cluster,
	}

	seedHost := opts.Host
	if opts.Port != "" {
		seedHost = fmt.Sprintf("%s:%s", opts.Host, opts.Port)
	}

	stat.AddNewNode(seedHost)

	// kick it off
	err = stat.Run()
	if err != nil {
		log.Logf(log.Always, "Error: %v", err)
		util.ExitFail()
		return
	}
}
