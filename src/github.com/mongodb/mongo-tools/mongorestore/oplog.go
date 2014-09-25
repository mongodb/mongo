package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"os"
	"time"
)

const OplogMaxCommandSize = 1024 * 1024 * 16.5

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

func (restore *MongoRestore) RestoreOplog() error {
	intent := restore.manager.Oplog()
	if intent == nil {
		log.Log(0, "no oplog.bson file in root of the dump directory, skipping oplog application")
	}

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("can't esablish session: %v", err)
	}
	session.SetSafe(restore.safety)
	defer session.Close()

	fileInfo, err := os.Lstat(intent.BSONPath)
	if err != nil {
		return fmt.Errorf("error reading bson file: %v", err)
	}
	size := fileInfo.Size()
	log.Logf(1, "\toplog %v is %v bytes", intent.BSONPath, size)

	oplogFile, err := os.Open(intent.BSONPath)
	if err != nil {
		return fmt.Errorf("error reading oplog file: %v", err)
	}

	bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(oplogFile))
	defer bsonSource.Close()

	entryArray := make([]bson.Raw, 0, 1024)
	rawOplogEntry := &bson.Raw{}

	var totalBytes, entrySize, bufferedBytes int

	bar := progress.ProgressBar{
		Max:        int(size),
		CounterPtr: &totalBytes,
		WaitTime:   3 * time.Second,
		Writer:     log.Writer(0),
		BarLength:  ProgressBarLength,
	}
	bar.Start()
	defer bar.Stop()

	// To restore the oplog, we iterate over the oplog entries,
	// filling up a buffer. Once the buffer reaches max document size,
	//
	//
	for bsonSource.Next(rawOplogEntry) {
		entrySize = len(rawOplogEntry.Data)
		if bufferedBytes+entrySize > OplogMaxCommandSize {
			err = ApplyOps(session, entryArray)
			if err != nil {
				return fmt.Errorf("error applying oplog: %v", err)
			}
			entryArray = make([]bson.Raw, 0, 1024)
			bufferedBytes = 0
		}

		bufferedBytes += entrySize
		totalBytes += entrySize
		entryArray = append(entryArray, *rawOplogEntry)
	}
	// finally, flush the remaining entries
	if len(entryArray) > 0 {
		err = ApplyOps(session, entryArray)
		if err != nil {
			return fmt.Errorf("error applying oplog: %v", err)
		}
	}

	return nil

}

func ApplyOps(session *mgo.Session, entries []bson.Raw) error {
	res := &bson.M{}
	err := session.Run(bson.M{"applyOps": entries}, res)
	if err != nil {
		return fmt.Errorf("applyOps: %v", err)
	}

	//log.Logf(0, "%+v", *res)
	return nil
}
