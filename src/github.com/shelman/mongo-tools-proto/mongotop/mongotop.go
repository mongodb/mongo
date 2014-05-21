package mongotop

import (
	"fmt"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/mongotop/options"
	"github.com/shelman/mongo-tools-proto/mongotop/output"
	"github.com/shelman/mongo-tools-proto/mongotop/poll"
	"time"
)

type MongoTop struct {
	// generic mongo tool options
	Options *commonopts.MongoToolOptions

	// mongotop-specific options
	TopOptions *options.MongoTopOptions

	// for polling the db
	poll.Poller

	// for outputting the results
	output.Outputter
}

func (self *MongoTop) Run() error {

	// make the initial connection
	connectionUrl, err := self.Poller.Connect()
	if err != nil {
		return fmt.Errorf("error connecting: %v", err)
	} else {
		fmt.Println(
			fmt.Sprintf("connected to %v", connectionUrl),
		)
	}

	for {

		// poll...
		pollResults, err := self.Poller.Poll()
		if err != nil {
			return fmt.Errorf("error polling database: %v", err)
		}

		// output...
		if err := self.Outputter.Output(pollResults,
			self.Options); err != nil {
			return fmt.Errorf("error outputting results: %v", err)
		}

		// sleep...
		time.Sleep(time.Duration(self.TopOptions.SleepTime) * time.Second)
	}

	return nil
}
