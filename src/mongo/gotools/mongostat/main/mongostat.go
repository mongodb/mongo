// Main package for the mongostat tool.
package main

import (
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/password"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongostat"
	"github.com/mongodb/mongo-tools/mongostat/stat_consumer"
	"github.com/mongodb/mongo-tools/mongostat/stat_consumer/line"
	"github.com/mongodb/mongo-tools/mongostat/status"
)

// optionKeyNames interprets the CLI options Columns and AppendColumns into
// the internal keyName mapping.
func optionKeyNames(option string) map[string]string {
	kn := make(map[string]string)
	columns := strings.Split(option, ",")
	for _, column := range columns {
		naming := strings.Split(column, "=")
		if len(naming) == 1 {
			kn[naming[0]] = naming[0]
		} else {
			kn[naming[0]] = naming[1]
		}
	}
	return kn
}

// optionCustomHeaders interprets the CLI options Columns and AppendColumns
// into a list of custom headers.
func optionCustomHeaders(option string) (headers []string) {
	columns := strings.Split(option, ",")
	for _, column := range columns {
		naming := strings.Split(column, "=")
		headers = append(headers, naming[0])
	}
	return
}

func main() {
	// initialize command-line opts
	opts := options.New(
		"mongostat",
		mongostat.Usage,
		options.EnabledOptions{Connection: true, Auth: true, Namespace: false})
	opts.UseReadOnlyHostDescription()

	// add mongostat-specific options
	statOpts := &mongostat.StatOptions{}
	opts.AddOptions(statOpts)

	interactiveOption := opts.FindOptionByLongName("interactive")
	if _, available := stat_consumer.FormatterConstructors["interactive"]; !available {
		// make --interactive inaccessible
		interactiveOption.LongName = ""
		interactiveOption.ShortName = 0
	}

	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'mongostat --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	log.SetVerbosity(opts.Verbosity)
	signals.Handle()

	sleepInterval := 1
	if len(args) > 0 {
		if len(args) != 1 {
			log.Logvf(log.Always, "too many positional arguments: %v", args)
			log.Logvf(log.Always, "try 'mongostat --help' for more information")
			os.Exit(util.ExitBadOptions)
		}
		sleepInterval, err = strconv.Atoi(args[0])
		if err != nil {
			log.Logvf(log.Always, "invalid sleep interval: %v", args[0])
			os.Exit(util.ExitBadOptions)
		}
		if sleepInterval < 1 {
			log.Logvf(log.Always, "sleep interval must be at least 1 second")
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

	if opts.Auth.Username != "" && opts.Auth.Source == "" && !opts.Auth.RequiresExternalDB() {
		log.Logvf(log.Always, "--authenticationDatabase is required when authenticating against a non $external database")
		os.Exit(util.ExitBadOptions)
	}

	if statOpts.Interactive && statOpts.Json {
		log.Logvf(log.Always, "cannot use output formats --json and --interactive together")
		os.Exit(util.ExitBadOptions)
	}

	if statOpts.Deprecated && !statOpts.Json {
		log.Logvf(log.Always, "--useDeprecatedJsonKeys can only be used when --json is also specified")
		os.Exit(util.ExitBadOptions)
	}

	if statOpts.Columns != "" && statOpts.AppendColumns != "" {
		log.Logvf(log.Always, "-O cannot be used if -o is also specified")
		os.Exit(util.ExitBadOptions)
	}

	if statOpts.HumanReadable != "true" && statOpts.HumanReadable != "false" {
		log.Logvf(log.Always, "--humanReadable must be set to either 'true' or 'false'")
		os.Exit(util.ExitBadOptions)
	}

	// we have to check this here, otherwise the user will be prompted
	// for a password for each discovered node
	if opts.Auth.ShouldAskForPassword() {
		opts.Auth.Password = password.Prompt()
	}

	var factory stat_consumer.FormatterConstructor
	if statOpts.Json {
		factory = stat_consumer.FormatterConstructors["json"]
	} else if statOpts.Interactive {
		factory = stat_consumer.FormatterConstructors["interactive"]
	} else {
		factory = stat_consumer.FormatterConstructors[""]
	}
	formatter := factory(statOpts.RowCount, !statOpts.NoHeaders)

	cliFlags := 0
	if statOpts.Columns == "" {
		cliFlags = line.FlagAlways
		if statOpts.Discover {
			cliFlags |= line.FlagDiscover
			cliFlags |= line.FlagHosts
		}
		if statOpts.All {
			cliFlags |= line.FlagAll
		}
		if strings.Contains(opts.Host, ",") {
			cliFlags |= line.FlagHosts
		}
	}

	var customHeaders []string
	if statOpts.Columns != "" {
		customHeaders = optionCustomHeaders(statOpts.Columns)
	} else if statOpts.AppendColumns != "" {
		customHeaders = optionCustomHeaders(statOpts.AppendColumns)
	}

	var keyNames map[string]string
	if statOpts.Deprecated {
		keyNames = line.DeprecatedKeyMap()
	} else if statOpts.Columns == "" {
		keyNames = line.DefaultKeyMap()
	} else {
		keyNames = optionKeyNames(statOpts.Columns)
	}
	if statOpts.AppendColumns != "" {
		addKN := optionKeyNames(statOpts.AppendColumns)
		for k, v := range addKN {
			keyNames[k] = v
		}
	}

	readerConfig := &status.ReaderConfig{
		HumanReadable: statOpts.HumanReadable == "true",
	}
	if statOpts.Json {
		readerConfig.TimeFormat = "15:04:05"
	}

	consumer := stat_consumer.NewStatConsumer(cliFlags, customHeaders,
		keyNames, readerConfig, formatter, os.Stdout)
	seedHosts := util.CreateConnectionAddrs(opts.Host, opts.Port)
	var cluster mongostat.ClusterMonitor
	if statOpts.Discover || len(seedHosts) > 1 {
		cluster = &mongostat.AsyncClusterMonitor{
			ReportChan:    make(chan *status.ServerStatus),
			ErrorChan:     make(chan *status.NodeError),
			LastStatLines: map[string]*line.StatLine{},
			Consumer:      consumer,
		}
	} else {
		cluster = &mongostat.SyncClusterMonitor{
			ReportChan: make(chan *status.ServerStatus),
			ErrorChan:  make(chan *status.NodeError),
			Consumer:   consumer,
		}
	}

	var discoverChan chan string
	if statOpts.Discover {
		discoverChan = make(chan string, 128)
	}

	opts.Direct = true
	_, setName := util.ParseConnectionString(opts.Host)
	opts.ReplicaSetName = setName
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
	formatter.Finish()
	if err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}
}
