package mongotop

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
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

	if err := db.ConfirmConnect(); err != nil {
		msg := fmt.Sprintf("error connecting to db: %v", err)
		fmt.Println(msg)
		return fmt.Errorf(msg)
	} else {
		fmt.Println(
			fmt.Sprintf("connected to %v", db.Url()),
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
