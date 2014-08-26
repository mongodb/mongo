// Package mongotop implements the core logic and structures
// for the mongotop tool.
package mongotop

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongotop/command"
	"github.com/shelman/mongo-tools-proto/mongotop/options"
	"github.com/shelman/mongo-tools-proto/mongotop/output"
	"time"
)

// Wrapper for the mongotop functionality
type MongoTop struct {
	// generic mongo tool options
	Options *commonopts.ToolOptions

	// mongotop-specific output options
	OutputOptions *options.Output

	// for connecting to the db
	SessionProvider *db.SessionProvider

	// for outputting the results
	output.Outputter

	// the sleep time
	Sleeptime time.Duration

	// just run once and finish
	Once bool
}

// Connect to the database and spin, running the top command and outputting
// the results appropriately.
func (self *MongoTop) Run() error {

	// test the connection
	session, err := self.SessionProvider.GetSession()
	if err != nil {
		return err
	}
	session.Close()

	connUrl := self.Options.Host
	if self.Options.Port != "" {
		connUrl = connUrl + ":" + self.Options.Port
	}
	util.Printlnf("connected to: %v", connUrl)

	// the results used to be compared to each other
	var previousResults command.Command
	if self.OutputOptions.Locks {
		previousResults = &command.ServerStatus{}
	} else {
		previousResults = &command.Top{}
	}

	// populate the first run of the previous results
	err = self.SessionProvider.RunCommand("admin", previousResults)
	if err != nil {
		return fmt.Errorf("error running top command: %v", err)
	}

	for {

		// sleep
		time.Sleep(self.Sleeptime)

		var topResults command.Command
		if self.OutputOptions.Locks {
			topResults = &command.ServerStatus{}
		} else {
			topResults = &command.Top{}
		}

		// run the top command against the database
		err = self.SessionProvider.RunCommand("admin", topResults)
		if err != nil {
			return fmt.Errorf("error running top command: %v", err)
		}

		// diff the results
		diff, err := topResults.Diff(previousResults)
		if err != nil {
			return fmt.Errorf("error computing diff: %v", err)
		}

		// output the results
		if err := self.Outputter.Output(diff); err != nil {
			return fmt.Errorf("error outputting results: %v", err)
		}

		// update the previous results
		previousResults = topResults

		if self.Once {
			return nil
		}

	}

}
