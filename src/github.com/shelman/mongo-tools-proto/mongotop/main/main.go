package main

import (
	"fmt"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/mongotop"
	"github.com/shelman/mongo-tools-proto/mongotop/options"
	"github.com/shelman/mongo-tools-proto/mongotop/output"
	"github.com/shelman/mongo-tools-proto/mongotop/poll"
)

func main() {

	opts := commonopts.GetMongoToolOptions()
	topOpts := &options.MongoTopOptions{}
	opts.AddOptions(topOpts)
	err := opts.Parse()
	if err != nil {
		fmt.Println(fmt.Sprintf("Error: %v", err))
	}

	if opts.Help {
		opts.Usage()
		return
	}

	if opts.Version {
		fmt.Println("should print mongotop version")
		return
	}

	top := &mongotop.MongoTop{
		Options:    opts,
		TopOptions: topOpts,
		Poller:     &poll.DBPoller{},
		Outputter:  &output.TerminalOutputter{},
	}

	top.Run()
}
