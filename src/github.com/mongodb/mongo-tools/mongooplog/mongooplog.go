package mongooplog

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongooplog/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"time"
)

type MongoOplog struct {
	// standard tool options
	ToolOptions *commonopts.ToolOptions

	// mongooplog-specific options
	SourceOptions *options.SourceOptions

	// session provider for the source server
	SessionProviderFrom *db.SessionProvider

	// command runner for the destination server
	CmdRunnerTo db.CommandRunner
}

// CreateCommandRunner returns either a shim or a session provider, based
// on whether --dbpath is set.
func CreateCommandRunner(opts *commonopts.ToolOptions) (db.CommandRunner,
	error) {

	// handle --dbpath, if appropriate
	if opts.Namespace.DBPath != "" {
		shim, err := db.NewShim(opts.Namespace.DBPath, opts.DirectoryPerDB,
			opts.Journal)
		if err != nil {
			return nil, fmt.Errorf("error attaching to --dbpath: %v", err)
		}

		// shim successfully created
		return shim, nil
	}

	// if --dbpath is not used, create a session provider to connect to
	// the server
	return db.NewSessionProvider(*opts), nil
}

func (self *MongoOplog) Run() error {

	// split up the oplog namespace we are using
	oplogDB, oplogColl, err :=
		util.SplitAndValidateNamespace(self.SourceOptions.OplogNS)

	if err != nil {
		return err
	}

	// the full oplog namespace needs to be specified
	if oplogColl == "" {
		return fmt.Errorf("the oplog namespace must specify a collection")
	}

	// connect to the source server
	fromSession, err := self.SessionProviderFrom.GetSession()
	if err != nil {
		return fmt.Errorf("error connecting to source db: %v", err)
	}
	defer fromSession.Close()

	// get the tailing cursor for the source server's oplog
	tail := buildTailingCursor(fromSession.DB(oplogDB).C(oplogColl),
		self.SourceOptions)
	defer tail.Close()

	// read the cursor dry, applying ops to the destination
	// server in the process
	oplogEntry := &OplogEntry{}
	res := &ApplyOpsResponse{}

	for tail.Next(oplogEntry) {

		// make sure there was no tailing error
		if err := tail.Err(); err != nil {
			return fmt.Errorf("error querying oplog: %v", err)
		}

		// skip noops
		if oplogEntry.Operation == "n" {
			continue
		}

		// prepare the op to be applied
		opsToApply := []OplogEntry{*oplogEntry}

		// apply the operation
		err := self.CmdRunnerTo.Run(bson.M{"applyOps": opsToApply},
			res, "admin")

		if err != nil {
			return fmt.Errorf("error applying ops: %v", err)
		}

		// check the server's response for an issue
		if !res.Ok {
			return fmt.Errorf("server gave error applying ops: %v", res.ErrMsg)
		}
	}

	return nil
}

// TODO: move this to common
type ApplyOpsResponse struct {
	Ok     bool   `bson:"ok"`
	ErrMsg string `bson:"errmsg"`
}

// TODO: move this to common / merge with the one in mongodump
type OplogEntry struct {
	Timestamp bson.MongoTimestamp `bson:"ts" json:"ts"`
	HistoryID int64               `bson:"h" json:"h"`
	Version   int                 `bson:"v" json:"v"`
	Operation string              `bson:"op" json:"op"`
	Namespace string              `bson:"ns" json:"ns"`
	Object    bson.M              `bson:"o" json:"o"`
	Query     bson.M              `bson:"o2" json:"o2"`
}

// get the tailing cursor for the oplog collection, based on the options
// passed in to mongooplog
func buildTailingCursor(oplog *mgo.Collection,
	sourceOptions *options.SourceOptions) *mgo.Iter {

	// how many seconds in the past we need
	secondsInPast := time.Duration(sourceOptions.Seconds) * time.Second
	// the time threshold for oplog queries
	threshold := time.Now().Add(-secondsInPast)
	// convert to a unix timestamp (seconds since epoch)
	thresholdAsUnix := threshold.Unix()

	// shift it appropriately, to prepare it to be converted to an
	// oplog timestamp
	thresholdShifted := uint64(thresholdAsUnix) << 32

	// build the oplog query
	oplogQuery := bson.M{
		"ts": bson.M{
			"$gte": bson.MongoTimestamp(thresholdShifted),
		},
	}

	// TODO: wait time
	return oplog.Find(oplogQuery).Tail(3)

}
