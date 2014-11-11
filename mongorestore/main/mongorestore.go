package main

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongorestore"
	"github.com/mongodb/mongo-tools/mongorestore/options"
	"os"
	"runtime"
)

func main() {
	// initialize command-line opts
	opts := commonopts.New("mongorestore", "<options>")
	inputOpts := &options.InputOptions{}
	opts.AddOptions(inputOpts)
	outputOpts := &options.OutputOptions{}
	opts.AddOptions(outputOpts)

	extraArgs, err := opts.Parse()
	if err != nil {
		fmt.Printf("error parsing command line options: %v\n\n", err)
		fmt.Printf("try 'mongorestore --help' for more information\n")
		os.Exit(2)
	}

	// print help or version info, if specified
	if opts.PrintHelp(false) {
		return
	}
	if opts.PrintVersion() {
		return
	}

	log.SetVerbosity(opts.Verbosity)

	runtime.GOMAXPROCS(runtime.NumCPU())
	log.Logf(log.Info, "running mongorestore with %v job threads", outputOpts.JobThreads)

	targetDir, err := getTargetDirFromArgs(extraArgs, os.Args, inputOpts.Directory)
	if err != nil {
		fmt.Printf("error parsing command line options: %v\n", err)
		os.Exit(2)
	}
	targetDir = util.ToUniversalPath(targetDir)

	opts.Direct = true

	restore := mongorestore.MongoRestore{
		ToolOptions:     opts,
		OutputOptions:   outputOpts,
		InputOptions:    inputOpts,
		TargetDirectory: targetDir,
		SessionProvider: db.NewSessionProvider(*opts),
	}

	err = restore.Restore()
	if err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		util.ExitFail()
		return
	}
}

// getTargetDirFromArgs handles the logic and error cases of figuring out
// the target restore directory.
func getTargetDirFromArgs(extraArgs, osArgs []string, dirFlag string) (string, error) {
	// This logic is in a switch statement so that the rules are understandable, the
	// code below could be refactored for compactness at the expense of readability.
	// We start by handling error cases, and then handle the different ways the target
	// directory can be legally set.
	switch {
	case len(extraArgs) > 1:
		// error on cases when there are too many positional arguments
		fallthrough
	case len(extraArgs) == 1 && (osArgs[len(osArgs)-1] == "-" || osArgs[len(osArgs)-2] == "-"):
		// handle the special case where stdin (-) is passed in along with another argument
		return "", fmt.Errorf("too many positional arguments")

	case dirFlag != "" && (len(extraArgs) == 1 || osArgs[len(osArgs)-1] == "-"):
		// error when positional arguments and --dir are used
		return "", fmt.Errorf(
			"cannot use both --dir and a positional argument to set the target directory")

	case len(extraArgs) == 1:
		// a nice, simple case where one argument is given, so we use it
		return extraArgs[0], nil

	case len(extraArgs) == 0 && osArgs[len(osArgs)-1] == "-":
		// we have to do a manual os.Args check due to edge case logic in our
		// go-flags library that prevents a single dash from bubbling up
		return "-", nil

	case len(extraArgs) == 0 && dirFlag != "":
		// if we have no extra args and a --dir flag, use the --dir flag
		log.Log(log.Info, "using --dir flag instead of arguments")
		return dirFlag, nil

	default:
		log.Log(log.Info, "using default 'dump' directory")
		return "dump", nil
	}
}
