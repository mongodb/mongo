package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongostat"
	"os"
	"strconv"
	"strings"
	"time"
)

func main() {
	// initialize command-line opts
	opts := options.New(
		"mongostat",
		"[options] <polling interval in seconds>",
		options.EnabledOptions{Connection: true, Auth: true, Namespace: false})

	// add mongostat-specific options
	statOpts := &mongostat.StatOptions{}
	opts.AddOptions(statOpts)

	extra, err := opts.Parse()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Invalid options: %v\n", err)
		opts.PrintHelp(true)
		os.Exit(util.ExitBadOptions)
	}

	log.SetVerbosity(opts.Verbosity)

	sleepInterval := 1
	if len(extra) > 0 {
		if len(extra) != 1 {
			fmt.Fprintf(os.Stderr, "Too many positional operators\n")
			opts.PrintHelp(true)
			os.Exit(util.ExitBadOptions)
		}
		sleepInterval, err = strconv.Atoi(extra[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "Bad sleep interval: %v\n", extra[0])
			os.Exit(util.ExitBadOptions)
		}
		if sleepInterval < 1 {
			fmt.Fprintf(os.Stderr, "Sleep interval must be at least 1 second\n")
			os.Exit(util.ExitBadOptions)
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

	seedHosts := []string{}
	hostOption := strings.Split(opts.Host, ",")
	for _, seedHost := range hostOption {
		if opts.Port != "" {
			seedHost = fmt.Sprintf("%s:%s", seedHost, opts.Port)
		}
		seedHosts = append(seedHosts, seedHost)
	}

	var cluster mongostat.ClusterMonitor
	if statOpts.Discover || len(seedHosts) > 1 {
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
		Cluster:       cluster,
	}

	for _, v := range seedHosts {
		stat.AddNewNode(v)
	}

	// kick it off
	err = stat.Run()
	if err != nil {
		log.Logf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
}
