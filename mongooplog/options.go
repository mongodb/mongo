package mongooplog

import (
	"fmt"
	"gopkg.in/mgo.v2/bson"
)

type SourceOptions struct {
	From    string              `long:"from" description:"specify the host for mongooplog to retrive operations from"`
	OplogNS string              `long:"oplogns" description:"specify the namespace in the --from host where the oplog lives" default:"local.oplog.rs"`
	Seconds bson.MongoTimestamp `long:"seconds" short:"s" description:"specify a number of seconds for mongooplog to pull from the remote host" default:"86400"`
}

func (self *SourceOptions) Name() string {
	return "source"
}

func (self *SourceOptions) Validate() error {
	if self.From == "" {
		return fmt.Errorf("need to specify --from")
	}
	return nil
}
