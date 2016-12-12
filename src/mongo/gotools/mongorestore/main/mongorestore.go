// Main package for the mongorestore tool.
package main

import (
	"fmt"
	"os"
	"time"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/signals"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongorestore"
)

const (
	progressBarLength   = 24
	progressBarWaitTime = time.Second * 3
)

func main() {
	// initialize command-line opts
	opts := options.New("mongorestore", mongorestore.Usage,
		options.EnabledOptions{Auth: true, Connection: true})
	nsOpts := &mongorestore.NSOptions{}
	opts.AddOptions(nsOpts)
	inputOpts := &mongorestore.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &mongorestore.OutputOptions{}
	opts.AddOptions(outputOpts)

	extraArgs, err := opts.Parse()
	if err != nil {
		log.Logvf(log.Always, "error parsing command line options: %v", err)
		log.Logvf(log.Always, "try 'mongorestore --help' for more information")
		os.Exit(util.ExitBadOptions)
	}

	// Allow the db connector to fall back onto the current database when no
	// auth database is given; the standard -d/-c options go into nsOpts now
	opts.Namespace = &options.Namespace{DB: nsOpts.DB}

	// print help or version info, if specified
	if opts.PrintHelp(false) {
		return
	}

	if opts.PrintVersion() {
		return
	}

	log.SetVerbosity(opts.Verbosity)

	targetDir, err := getTargetDirFromArgs(extraArgs, inputOpts.Directory)
	if err != nil {
		log.Logvf(log.Always, "%v", err)
		log.Logvf(log.Always, "try 'mongorestore --help' for more information")
		os.Exit(util.ExitBadOptions)
	}
	targetDir = util.ToUniversalPath(targetDir)

	// connect directly, unless a replica set name is explicitly specified
	_, setName := util.ParseConnectionString(opts.Host)
	opts.Direct = (setName == "")
	opts.ReplicaSetName = setName

	provider, err := db.NewSessionProvider(*opts)
	defer provider.Close()
	if err != nil {
		log.Logvf(log.Always, "error connecting to host: %v", err)
		os.Exit(util.ExitError)
	}
	provider.SetBypassDocumentValidation(outputOpts.BypassDocumentValidation)

	// disable TCP timeouts for restore jobs
	provider.SetFlags(db.DisableSocketTimeout)

	// start up the progress bar manager
	progressManager := progress.NewBarWriter(log.Writer(0), progressBarWaitTime, progressBarLength, true)
	progressManager.Start()
	defer progressManager.Stop()

	restore := mongorestore.MongoRestore{
		ToolOptions:     opts,
		OutputOptions:   outputOpts,
		InputOptions:    inputOpts,
		NSOptions:       nsOpts,
		TargetDirectory: targetDir,
		SessionProvider: provider,
		ProgressManager: progressManager,
	}

	finishedChan := signals.HandleWithInterrupt(restore.HandleInterrupt)
	defer close(finishedChan)

	if err = restore.Restore(); err != nil {
		log.Logvf(log.Always, "Failed: %v", err)
		if err == util.ErrTerminated {
			os.Exit(util.ExitKill)
		}
		os.Exit(util.ExitError)
	}
}

// getTargetDirFromArgs handles the logic and error cases of figuring out
// the target restore directory.
func getTargetDirFromArgs(extraArgs []string, dirFlag string) (string, error) {
	// This logic is in a switch statement so that the rules are understandable.
	// We start by handling error cases, and then handle the different ways the target
	// directory can be legally set.
	switch {
	case len(extraArgs) > 1:
		// error on cases when there are too many positional arguments
		return "", fmt.Errorf("too many positional arguments")

	case dirFlag != "" && len(extraArgs) > 0:
		// error when positional arguments and --dir are used
		return "", fmt.Errorf(
			"cannot use both --dir and a positional argument to set the target directory")

	case len(extraArgs) == 1:
		// a nice, simple case where one argument is given, so we use it
		return extraArgs[0], nil

	case dirFlag != "":
		// if we have no extra args and a --dir flag, use the --dir flag
		log.Logv(log.Info, "using --dir flag instead of arguments")
		return dirFlag, nil

	default:
		return "", nil
	}
}
