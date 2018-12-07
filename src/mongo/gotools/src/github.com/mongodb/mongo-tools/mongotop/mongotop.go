// Package mongotop provides a method to track the amount of time a MongoDB instance spends reading and writing data.
package mongotop

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"time"
)

// MongoTop is a container for the user-specified options and
// internal state used for running mongotop.
type MongoTop struct {
	// Generic mongo tool options
	Options *options.ToolOptions

	// Mongotop-specific output options
	OutputOptions *Output

	// for connecting to the db
	SessionProvider *db.SessionProvider

	// Length of time to sleep between each polling.
	Sleeptime time.Duration

	previousServerStatus *ServerStatus
	previousTop          *Top
}

func (mt *MongoTop) runDiff() (outDiff FormattableDiff, err error) {
	session, err := mt.SessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()
	session.SetSocketTimeout(0)

	var currentServerStatus ServerStatus
	var currentTop Top
	commandName := "top"
	var dest interface{} = &currentTop
	if mt.OutputOptions.Locks {
		commandName = "serverStatus"
		dest = &currentServerStatus
	}
	err = session.DB("admin").Run(commandName, dest)
	if err != nil {
		mt.previousServerStatus = nil
		mt.previousTop = nil
		return nil, err
	}
	if mt.OutputOptions.Locks {
		if currentServerStatus.Locks == nil {
			return nil, fmt.Errorf("server does not support reporting lock information")
		}
		for _, ns := range currentServerStatus.Locks {
			if ns.AcquireCount != nil {
				return nil, fmt.Errorf("server does not support reporting lock information")
			}
		}
		if mt.previousServerStatus != nil {
			serverStatusDiff := currentServerStatus.Diff(*mt.previousServerStatus)
			outDiff = serverStatusDiff
		}
		mt.previousServerStatus = &currentServerStatus
	} else {
		if mt.previousTop != nil {
			topDiff := currentTop.Diff(*mt.previousTop)
			outDiff = topDiff
		}
		mt.previousTop = &currentTop
	}
	return outDiff, nil
}

// Run executes the mongotop program.
func (mt *MongoTop) Run() error {

	connURL := mt.Options.Host
	if connURL == "" {
		connURL = "127.0.0.1"
	}
	if mt.Options.Port != "" {
		connURL = connURL + ":" + mt.Options.Port
	}

	hasData := false
	numPrinted := 0

	for {
		if mt.OutputOptions.RowCount > 0 && numPrinted > mt.OutputOptions.RowCount {
			return nil
		}
		numPrinted++
		diff, err := mt.runDiff()
		if err != nil {
			// If this is the first time trying to poll the server and it fails,
			// just stop now instead of trying over and over.
			if !hasData {
				return err
			}

			log.Logvf(log.Always, "Error: %v\n", err)
			time.Sleep(mt.Sleeptime)
		}

		// if this is the first time and the connection is successful, print
		// the connection message
		if !hasData && !mt.OutputOptions.Json {
			log.Logvf(log.Always, "connected to: %v\n", connURL)
		}

		hasData = true

		if diff != nil {
			if mt.OutputOptions.Json {
				fmt.Println(diff.JSON())
			} else {
				fmt.Println(diff.Grid())
			}
		}
		time.Sleep(mt.Sleeptime)
	}
}
