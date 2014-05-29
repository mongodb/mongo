package options

import (
	//	"flag"
	"fmt"
	//	flag "github.com/ogier/pflag"
	"os"
	"strconv"
	"strings"
)

type MongoTopOptions struct {
	Locks      bool `long:"locks" description:"Report on use of per-database locks"`
	DB         string
	Collection string
	SleepTime  int
}

func (self *MongoTopOptions) Name() string {
	return "mongotop"
}

func (self *MongoTopOptions) PostParse() error {

	// TODO: clean this up
	// get the sleep time, which is the final command line arg
	self.SleepTime = 1
	args := os.Args
	if len(args) >= 2 {
		lastArg := args[len(args)-1]
		secondToLastArg := args[len(args)-2]
		// TODO: this can be done much better
		if !strings.HasPrefix(lastArg, "-") &&
			!strings.Contains(secondToLastArg, "-port") &&
			!strings.Contains(secondToLastArg, "-p") &&
			!strings.Contains(secondToLastArg, "-host") &&
			!strings.Contains(secondToLastArg, "-h") {
			sleepTime, err := strconv.Atoi(lastArg)
			if err != nil {
				return fmt.Errorf("bad sleep time: %v", lastArg)
			}
			self.SleepTime = sleepTime
		}
	}

	return nil
}

func (self *MongoTopOptions) Validate() error {
	// unimplemented ftm
	return nil
}
