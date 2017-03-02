// Package bongooplog polls operations from the replication oplog of one server, and applies them to another.
package bongooplog

import (
	"fmt"
	"github.com/bongodb/bongo-tools/common/db"
	"github.com/bongodb/bongo-tools/common/log"
	"github.com/bongodb/bongo-tools/common/options"
	"github.com/bongodb/bongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"time"
)

// BongoOplog is a container for the user-specified options for running bongooplog.
type BongoOplog struct {
	// standard tool options
	ToolOptions *options.ToolOptions

	// bongooplog-specific options
	SourceOptions *SourceOptions

	// session provider for the source server
	SessionProviderFrom *db.SessionProvider

	// session provider for the destination server
	SessionProviderTo *db.SessionProvider
}

// Run executes the bongooplog program.
func (mo *BongoOplog) Run() error {

	// split up the oplog namespace we are using
	oplogDB, oplogColl, err :=
		util.SplitAndValidateNamespace(mo.SourceOptions.OplogNS)

	if err != nil {
		return err
	}

	// the full oplog namespace needs to be specified
	if oplogColl == "" {
		return fmt.Errorf("the oplog namespace must specify a collection")
	}

	log.Logvf(log.DebugLow, "using oplog namespace `%v.%v`", oplogDB, oplogColl)

	// connect to the destination server
	toSession, err := mo.SessionProviderTo.GetSession()
	if err != nil {
		return fmt.Errorf("error connecting to destination db: %v", err)
	}
	defer toSession.Close()
	toSession.SetSocketTimeout(0)

	// purely for logging
	destServerStr := mo.ToolOptions.Host
	if mo.ToolOptions.Port != "" {
		destServerStr = destServerStr + ":" + mo.ToolOptions.Port
	}
	log.Logvf(log.DebugLow, "successfully connected to destination server `%v`", destServerStr)

	// connect to the source server
	fromSession, err := mo.SessionProviderFrom.GetSession()
	if err != nil {
		return fmt.Errorf("error connecting to source db: %v", err)
	}
	defer fromSession.Close()
	fromSession.SetSocketTimeout(0)

	log.Logvf(log.DebugLow, "successfully connected to source server `%v`", mo.SourceOptions.From)

	// set slave ok
	fromSession.SetMode(mgo.Eventual, true)

	// get the tailing cursor for the source server's oplog
	tail := buildTailingCursor(fromSession.DB(oplogDB).C(oplogColl),
		mo.SourceOptions)
	defer tail.Close()

	// read the cursor dry, applying ops to the destination
	// server in the process
	oplogEntry := &db.Oplog{}
	res := &db.ApplyOpsResponse{}

	log.Logv(log.DebugLow, "applying oplog entries...")

	opCount := 0

	for tail.Next(oplogEntry) {

		// skip noops
		if oplogEntry.Operation == "n" {
			log.Logvf(log.DebugHigh, "skipping no-op for namespace `%v`", oplogEntry.Namespace)
			continue
		}
		opCount++

		// prepare the op to be applied
		opsToApply := []db.Oplog{*oplogEntry}

		// apply the operation
		err := toSession.Run(bson.M{"applyOps": opsToApply}, res)

		if err != nil {
			return fmt.Errorf("error applying ops: %v", err)
		}

		// check the server's response for an issue
		if !res.Ok {
			return fmt.Errorf("server gave error applying ops: %v", res.ErrMsg)
		}
	}

	// make sure there was no tailing error
	if err := tail.Err(); err != nil {
		return fmt.Errorf("error querying oplog: %v", err)
	}

	log.Logvf(log.DebugLow, "done applying %v oplog entries", opCount)

	return nil
}

// get the cursor for the oplog collection, based on the options
// passed in to bongooplog
func buildTailingCursor(oplog *mgo.Collection,
	sourceOptions *SourceOptions) *mgo.Iter {

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
			"$gte": bson.BongoTimestamp(thresholdShifted),
		},
	}

	// TODO: wait time
	return oplog.Find(oplogQuery).Iter()

}
