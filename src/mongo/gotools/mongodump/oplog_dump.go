package mongodump

import (
	"fmt"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
)

// determineOplogCollectionName uses a command to infer
// the name of the oplog collection in the connected db
func (dump *MongoDump) determineOplogCollectionName() error {
	masterDoc := bson.M{}
	err := dump.SessionProvider.Run("isMaster", &masterDoc, "admin")
	if err != nil {
		return fmt.Errorf("error running command: %v", err)
	}
	if _, ok := masterDoc["hosts"]; ok {
		log.Logvf(log.DebugLow, "determined cluster to be a replica set")
		log.Logvf(log.DebugHigh, "oplog located in local.oplog.rs")
		dump.oplogCollection = "oplog.rs"
		return nil
	}
	if isMaster := masterDoc["ismaster"]; util.IsFalsy(isMaster) {
		log.Logvf(log.Info, "mongodump is not connected to a master")
		return fmt.Errorf("not connected to master")
	}

	log.Logvf(log.DebugLow, "not connected to a replica set, assuming master/slave")
	log.Logvf(log.DebugHigh, "oplog located in local.oplog.$main")
	dump.oplogCollection = "oplog.$main"
	return nil

}

// getOplogStartTime returns the most recent oplog entry
func (dump *MongoDump) getOplogStartTime() (bson.MongoTimestamp, error) {
	mostRecentOplogEntry := db.Oplog{}

	err := dump.SessionProvider.FindOne("local", dump.oplogCollection, 0, nil, []string{"-$natural"}, &mostRecentOplogEntry, 0)
	if err != nil {
		return 0, err
	}
	return mostRecentOplogEntry.Timestamp, nil
}

// checkOplogTimestampExists checks to make sure the oplog hasn't rolled over
// since mongodump started. It does this by checking the oldest oplog entry
// still in the database and making sure it happened at or before the timestamp
// captured at the start of the dump.
func (dump *MongoDump) checkOplogTimestampExists(ts bson.MongoTimestamp) (bool, error) {
	oldestOplogEntry := db.Oplog{}
	err := dump.SessionProvider.FindOne("local", dump.oplogCollection, 0, nil, []string{"+$natural"}, &oldestOplogEntry, 0)
	if err != nil {
		return false, fmt.Errorf("unable to read entry from oplog: %v", err)
	}

	log.Logvf(log.DebugHigh, "oldest oplog entry has timestamp %v", oldestOplogEntry.Timestamp)
	if oldestOplogEntry.Timestamp > ts {
		log.Logvf(log.Info, "oldest oplog entry of timestamp %v is older than %v",
			oldestOplogEntry.Timestamp, ts)
		return false, nil
	}
	return true, nil
}

// DumpOplogAfterTimestamp takes a timestamp and writer and dumps all oplog entries after
// the given timestamp to the writer. Returns any errors that occur.
func (dump *MongoDump) DumpOplogAfterTimestamp(ts bson.MongoTimestamp) error {
	session, err := dump.SessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()
	session.SetPrefetch(1.0) // mimic exhaust cursor
	queryObj := bson.M{"ts": bson.M{"$gt": ts}}
	oplogQuery := session.DB("local").C(dump.oplogCollection).Find(queryObj).LogReplay()
	oplogCount, err := dump.dumpQueryToIntent(oplogQuery, dump.manager.Oplog(), dump.getResettableOutputBuffer())
	if err == nil {
		log.Logvf(log.Always, "\tdumped %v oplog %v",
			oplogCount, util.Pluralize(int(oplogCount), "entry", "entries"))
	}
	return err
}
