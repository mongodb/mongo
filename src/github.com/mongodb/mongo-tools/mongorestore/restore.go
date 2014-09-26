package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"gopkg.in/mgo.v2/bson"
	"io/ioutil"
	"os"
	"time"
)

const ProgressBarLength = 24

func (restore *MongoRestore) RestoreIntents() error {
	for intent := restore.manager.Pop(); intent != nil; intent = restore.manager.Pop() {
		err := restore.RestoreIntent(intent)
		if err != nil {
			return err
		}
	}
	return nil
}

func (restore *MongoRestore) RestoreIntent(intent *Intent) error {

	//session.SetSafe(restore.safety)

	collectionExists, err := restore.DBHasCollection(intent)
	if err != nil {
		return fmt.Errorf("error reading database: %v", err)
	}

	if restore.safety == nil && !restore.OutputOptions.Drop && collectionExists {
		log.Logf(0, "restoring to %v without dropping", intent.Key())
		log.Log(0, "IMPORTANT: restored data will be inserted without raising errors; check your server log")
	}

	if restore.OutputOptions.Drop {
		if collectionExists {
			log.Logf(1, "dropping collection %v before restoring", intent.Key())
			err = restore.cmdRunner.Run(bson.M{"drop": intent.C}, &bson.M{}, intent.DB) //TODO check result?
			if err != nil {
				return fmt.Errorf("error dropping collection: %v", err)
			}
			collectionExists = false
		} else {
			log.Logf(2, "collection %v doesn't exist, skipping drop command", intent.Key())
		}
	}

	var options bson.D
	var indexes []IndexDocument

	//first create collection with options
	if intent.MetadataPath != "" && !restore.OutputOptions.NoOptionsRestore {
		log.Logf(0, "reading metadata file from %v", intent.MetadataPath)
		jsonBytes, err := ioutil.ReadFile(intent.MetadataPath)
		if err != nil {
			return fmt.Errorf("error reading metadata file: %v", err) //TODO better errors here
		}
		options, indexes, err = restore.MetadataFromJSON(jsonBytes)
		if err != nil {
			return fmt.Errorf("error parsing metadata file (%v): %v", string(jsonBytes), err)
		}
		if options != nil {
			if collectionExists {
				log.Logf(1, "collection %v already exists", intent.Key())
			} else {
				log.Logf(1, "creating collection %v using options from metadata", intent.Key())
				err = restore.CreateCollection(intent, options)
				if err != nil {
					return fmt.Errorf("error creating collection %v: %v", intent.Key(), err)
				}
			}
		}
	}

	//then do bson
	if intent.BSONPath != "" {
		log.Logf(0, "restoring %v from file %v", intent.Key(), intent.BSONPath)

		fileInfo, err := os.Lstat(intent.BSONPath)
		if err != nil {
			return fmt.Errorf("error reading bson file: %v", err)
		}
		size := fileInfo.Size()
		log.Logf(1, "\tfile %v is %v bytes", intent.BSONPath, size)

		rawFile, err := os.Open(intent.BSONPath)
		if err != nil {
			return fmt.Errorf("error reading bson file: %v", err)
		}

		bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(rawFile))
		defer bsonSource.Close()

		err = restore.RestoreCollectionToDB(intent.DB, intent.C, bsonSource, size)
		if err != nil {
			return err
		}
	}

	//finally, add indexes
	if len(indexes) > 0 && !restore.OutputOptions.NoIndexRestore {
		log.Logf(0, "restoring indexes for collection %v from metadata", intent.Key())
		for _, idx := range indexes {
			log.Logf(0, "\tcreating index %v", idx.Options["name"])
			err = restore.InsertIndex(intent.DB, idx)
			if err != nil {
				return fmt.Errorf("error creating index %v: %v", idx.Options["name"], err)
			}
		}
	} else {
		log.Log(0, "no indexes to restore")
	}
	return nil
}

func (restore *MongoRestore) RestoreCollectionToDB(dbName, colName string,
	bsonSource *db.DecodedBSONSource, fileSize int64) error {

	insertStream, err := restore.cmdRunner.OpenInsertStream(dbName, colName, restore.safety)
	if err != nil {
		return fmt.Errorf("error opening insert connection: %v", err)
	}
	defer insertStream.Close()

	//progress bar handler
	bytesRead := 0
	bar := progress.ProgressBar{
		Max:        int(fileSize),
		CounterPtr: &bytesRead,
		WaitTime:   3 * time.Second,
		Writer:     log.Writer(0),
		BarLength:  ProgressBarLength,
	}
	bar.Start()
	defer bar.Stop()

	doc := &bson.Raw{}
	for bsonSource.Next(doc) {
		bytesRead += len(doc.Data)
		if restore.objCheck {
			//TODO encapsulate to reuse bson obj??
			err := bson.Unmarshal(doc.Data, &bson.M{})
			if err != nil {
				fmt.Println(err) //TODO
				break
			}
		}
		err := insertStream.WriteDoc(doc)
		if err != nil {
			return err
		}
	}
	if err = bsonSource.Err(); err != nil {
		return err
	}
	return nil
}
