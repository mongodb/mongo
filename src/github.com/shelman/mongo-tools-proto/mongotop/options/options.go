package options

import (
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"
)

type MongoTopOptions struct {
	Locks      bool
	DB         string
	Collection string
	SleepTime  int
}

func (self *MongoTopOptions) Usage() {
	fmt.Println("blecch... more usage")
}

func (self *MongoTopOptions) Register() {
	flag.BoolVar(&(self.Locks), "locks", false, "Report on lock usage")
}

func (self *MongoTopOptions) PostParse() error {
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
