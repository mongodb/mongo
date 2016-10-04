// Main package for the mongoexport tool.
package main

import (
	"os"
	"time"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoexport"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

const (
	progressBarLength   = 24
	progressBarWaitTime = time.Second
)

func main() {
	// initialize command-line opts
	opts := options.New("mongoexport", mongoexport.Usage,
		options.EnabledOptions{Auth: true, Connection: true, Namespace: true})

	outputOpts := &mongoexport.OutputFormatOptions{}
	opts.AddOptions(outputOpts)
	inputOpts := &mongoexport.InputOptions{}
	opts.AddOptions(inputOpts)

	args, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'mongoexport --help' for more information")
		os.Exit(util.ExitBadOptions)
	}
	if len(args) != 0 {
		log.Logvf(log.Always, "too many positional arguments: %v", args)
		log.Logvf(log.Always, "try 'mongoexport --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	log.SetVerbosity(opts.Verbosity)
	signals.Handle()

	// print help, if specified
	if opts.PrintHelp(false) {
		return
	}

	// print version, if specified
	if opts.PrintVersion() {
		return
	}

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")
	opts.ReplicaSetName = setName

	provider, err := db.NewSessionProvider(*opts)
	defer provider.Close()

	// temporarily allow secondary reads for the isMongos check
	provider.SetReadPreference(mgo.Nearest)
	isMongos, err := provider.IsMongos()
	if err != nil {
		log.Logvf(log.Always, "%v", err)
		os.Exit(util.ExitError)
	}

	provider.SetFlags(db.DisableSocketTimeout)

	if inputOpts.SlaveOk {
		if inputOpts.ReadPreference != "" {
			log.Logvf(log.Always, "--slaveOk can't be specified when --readPreference is specified")
			os.Exit(util.ExitBadOptions)
		}
		log.Logvf(log.Always, "--slaveOk is deprecated and being internally rewritten as --readPreference=nearest")
		inputOpts.ReadPreference = "nearest"
	}

	var mode mgo.Mode
	if opts.ReplicaSetName != "" || isMongos {
		mode = mgo.Primary
	} else {
		mode = mgo.Nearest
	}
	var tags bson.D
	if inputOpts.ReadPreference != "" {
		mode, tags, err = db.ParseReadPreference(inputOpts.ReadPreference)
		if err != nil {
			log.Logvf(log.Always, "error parsing --ReadPreference: %v", err)
			os.Exit(util.ExitBadOptions)
		}
		if len(tags) > 0 {
			provider.SetTags(tags)
		}
	}

	// warn if we are trying to export from a secondary in a sharded cluster
	if isMongos && mode != mgo.Primary {
		log.Logvf(log.Always, db.WarningNonPrimaryMongosConnection)
	}

	provider.SetReadPreference(mode)

	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}

	progressManager := progress.NewBarWriter(log.Writer(0), progressBarWaitTime, progressBarLength, false)
	progressManager.Start()
	defer progressManager.Stop()

	exporter := mongoexport.MongoExport{
		ToolOptions:     *opts,
		OutputOpts:      outputOpts,
		InputOpts:       inputOpts,
		SessionProvider: provider,
		ProgressManager: progressManager,
	}

	err = exporter.ValidateSettings()
	if err != nil {
		log.Logvf(log.Always, "error validating settings: %v", err)
		log.Logvf(log.Always, "try 'mongoexport --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	writer, err := exporter.GetOutputWriter()
	if err != nil {
		log.Logvf(log.Always, "error opening output stream: %v", err)
		os.Exit(util.ExitError)
	}
	if writer == nil {
		writer = os.Stdout
	} else {
		defer writer.Close()
	}

	numDocs, err := exporter.Export(writer)
	if err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		os.Exit(util.ExitError)
	}

	if numDocs == 1 {
		log.Logvf(log.Always, "exported %v record", numDocs)
	} else {
		log.Logvf(log.Always, "exported %v records", numDocs)
	}

}
