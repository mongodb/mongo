package mongooplog

import (
	"fmt"
	"gopkg.in/mgo.v2/bson"
)

var Usage = `--from <remote host> <options>

Poll operations from the replication oplog of one server, and apply them to another.

See http://docs.mongodb.org/manual/reference/program/mongooplog/ for more information.`

type SourceOptions struct {
	From    string              `long:"from" description:"specify the host for mongooplog to retrive operations from"`
	OplogNS string              `long:"oplogns" description:"specify the namespace in the --from host where the oplog lives (default 'local.oplog.rs') " default:"local.oplog.rs" default-mask:"-"`
	Seconds bson.MongoTimestamp `long:"seconds" short:"s" description:"specify a number of seconds for mongooplog to pull from the remote host" default:"86400"  default-mask:"-"`
}

func (_ *SourceOptions) Name() string {
	return "source"
}

func (o *SourceOptions) Validate() error {
	if o.From == "" {
		return fmt.Errorf("need to specify --from")
	}
	return nil
}
