package mongodump

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
)

//TODO move this to common if any of the other tools need it
type Oplog struct {
	Timestamp bson.MongoTimestamp `bson:"ts"`
	HistoryID int64               `bson:"h"`
	Version   int                 `bson:"v"`
	Operation string              `bson:"op"`
	Namespace string              `bson:"ns"`
	Object    bson.M              `bson:"o"`
	Query     bson.M              `bson:"o2"`
}

// determineOplogCollectionName uses a command to infer
// the name of the oplog collection in the connected db
func (dump *MongoDump) determineOplogCollectionName() error {
	masterDoc := bson.M{}
	fmt.Println("checking oplgo coll name")
	err := dump.cmdRunner.Run("isMaster", &masterDoc, "admin")
	if err != nil {
		return fmt.Errorf("error running command: %v", err)
	}
	if _, ok := masterDoc["hosts"]; ok {
		log.Logf(2, "determined cluster to be a replica set")
		log.Logf(3, "oplog located in local.oplog.rs")
		dump.oplogCollection = "oplog.rs"
		return nil
	}
	if isMaster := masterDoc["ismaster"]; util.IsFalsy(isMaster) {
		log.Logf(1, "mongodump is not connected to a master")
		return fmt.Errorf("not connected to master")
	}

	// TODO stop assuming master/slave, be smarter and check if it is really
	// master/slave...though to be fair legacy mongodump doesn't do this either...
	log.Logf(2, "not connected to a replica set, assuming master/slave")
	log.Logf(3, "oplog located in local.oplog.$main")
	dump.oplogCollection = "oplog.$main"
	return nil

}

// getOplogStartTime returns the most recent oplog entry
func (dump *MongoDump) getOplogStartTime() (bson.MongoTimestamp, error) {
	mostRecentOplogEntry := Oplog{}

	err := dump.cmdRunner.FindOne("local", dump.oplogCollection, 0, nil, []string{"-$natural"}, &mostRecentOplogEntry)
	if err != nil {
		//TODO different error depending on not found vs connection issues
		return 0, err
	}
	return mostRecentOplogEntry.Timestamp, nil
}

// checkOplogTimestampExists checks to make sure the oplog hasn't rolled over
// since mongodump started. It does this by checking the oldest oplog entry
// still in the database and making sure it happened at or before the timestamp
// captured at the start of the dump.
func (dump *MongoDump) checkOplogTimestampExists(ts bson.MongoTimestamp) (bool, error) {
	oldestOplogEntry := Oplog{}
	err := dump.cmdRunner.FindOne("local", dump.oplogCollection, 0, nil, []string{"+$natural"}, &oldestOplogEntry)
	if err != nil {
		return false, fmt.Errorf("unable to read entry from oplog: %v", err)
	}

	log.Logf(3, "oldest oplog entry has timestamp %v", oldestOplogEntry.Timestamp)
	if oldestOplogEntry.Timestamp > ts {
		log.Logf(1, "oldest oplog entry of timestamp %v is older than %v",
			oldestOplogEntry.Timestamp, ts)
		return false, nil
	}
	return true, nil
}
