package mongooplog

import (
	"gopkg.in/mgo.v2/bson"
)

var Usage = `--from <remote host> <options>

Poll operations from the replication oplog of one server, and apply them to another.

See http://docs.mongodb.org/manual/reference/program/mongooplog/ for more information.`

// SourceOptions defines the set of options to use in retrieving oplog data from the source server.
type SourceOptions struct {
	From    string              `long:"from" value-name:"<hostname>" description:"specify the host for mongooplog to retrive operations from"`
	OplogNS string              `long:"oplogns" value-name:"<namespace>" description:"specify the namespace in the --from host where the oplog lives (default 'local.oplog.rs') " default:"local.oplog.rs" default-mask:"-"`
	Seconds bson.MongoTimestamp `long:"seconds" value-name:"<seconds>" short:"s" description:"specify a number of seconds for mongooplog to pull from the remote host" default:"86400"  default-mask:"-"`
}

// Name returns a human-readable group name for source options.
func (_ *SourceOptions) Name() string {
	return "source"
}
