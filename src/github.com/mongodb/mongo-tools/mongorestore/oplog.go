package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
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
	log.Log(0, "replaying oplog")
	intent := restore.manager.Oplog()
	if intent == nil {
		log.Log(0, "no oplog.bson file in root of the dump directory, skipping oplog application")
	}

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

	entryArray := make([]interface{}, 0, 1024)
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
	// apply the current buffered ops and reset the buffer.
	// TODO use the new shim mode
	for bsonSource.Next(rawOplogEntry) {
		entrySize = len(rawOplogEntry.Data)
		if bufferedBytes+entrySize > OplogMaxCommandSize {
			err = restore.ApplyOps(entryArray)
			if err != nil {
				return fmt.Errorf("error applying oplog: %v", err)
			}
			entryArray = make([]interface{}, 0, 1024)
			bufferedBytes = 0
		}

		entryAsD := bson.D{}
		err = bson.Unmarshal(rawOplogEntry.Data, &entryAsD)
		if err != nil {
			return fmt.Errorf("error reading oplog: %v", err)
		}

		bufferedBytes += entrySize
		totalBytes += entrySize
		entryArray = append(entryArray, entryAsD)
	}
	// finally, flush the remaining entries
	if len(entryArray) > 0 {
		err = restore.ApplyOps(entryArray)
		if err != nil {
			return fmt.Errorf("error applying oplog: %v", err)
		}
	}

	return nil

}

// ApplyOps is a wrapper for the applyOps database command
func (restore *MongoRestore) ApplyOps(entries []interface{}) error {
	res := bson.M{}

	jsonCommand, err := bsonutil.ConvertBSONValueToJSON(
		bson.M{"applyOps": entries},
	)
	if err != nil {
		return err
	}

	err = restore.cmdRunner.Run(jsonCommand, &res, "admin")
	if err != nil {
		return fmt.Errorf("applyOps: %v", err)
	}
	if util.IsFalsy(res["ok"]) {
		return fmt.Errorf("applyOps command: %v", res["errmsg"])
	}

	return nil
}
