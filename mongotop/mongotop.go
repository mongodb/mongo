package mongotop

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongotop/options"
	"time"
)

// Wrapper for the mongotop functionality
type MongoTop struct {
	// Generic mongo tool options
	Options *commonopts.ToolOptions

	// Mongotop-specific output options
	OutputOptions *options.Output

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

// Connect to the database and periodically run a command to collect stats,
// writing the results to standard out in the specified format.
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
			log.Logf(log.Always, "Error: %v\n", err)

			// If this is the first time trying to poll the server and it fails,
			// just stop now instead of trying over and over.
			if !hasData {
				return err
			}
			time.Sleep(mt.Sleeptime)
		}

		// if this is the first time and the connection is successful, print
		// the connection message
		if !hasData && !mt.OutputOptions.Json {
			log.Logf(log.Always, "connected to: %v\n", connURL)
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
