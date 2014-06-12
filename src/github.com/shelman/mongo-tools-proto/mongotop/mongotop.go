// Package mongotop implements the core logic and structures
// for the mongotop tool.
package mongotop

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/mongotop/command"
	"github.com/shelman/mongo-tools-proto/mongotop/output"
	"time"
)

// Wrapper for the mongotop functionality
type MongoTop struct {
	// generic mongo tool options
	Options *commonopts.ToolOptions

	// for connecting to the db
	SessionProvider *db.SessionProvider

	// for outputting the results
	output.Outputter

	// the sleep time
	Sleeptime time.Duration
}

// Connect to the database and spin, running the top command and outputting
// the results appropriately.
func (self *MongoTop) Run() error {

	// the results from the previous run, used for diffing
	previousResults := &command.Top{}
	err := self.SessionProvider.RunCommand("admin", previousResults)
	if err != nil {
		return fmt.Errorf("error running top command: %v", err)
	}

	for {

		// sleep
		time.Sleep(self.Sleeptime)

		// run the top command against the database
		topResults := &command.Top{}
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

	}

}
