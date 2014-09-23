package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"gopkg.in/mgo.v2"
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

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("can't esablish session: %v", err)
	}
	session.SetSafe(restore.safety)
	defer session.Close()
	c := session.DB(intent.DB).C(intent.C)

	collectionExists, err := DBHasCollection(session.DB(intent.DB), intent.Key())
	if err != nil {
		return fmt.Errorf("error reading database: %v", err)
	}

	if restore.OutputOptions.Drop {
		if collectionExists {
			log.Logf(2, "collection %v doesn't exist, skipping drop command", intent.Key())
		} else {
			log.Logf(1, "dropping collection %v before restoring", intent.Key())
			err = c.DropCollection()
			if err != nil {
				return fmt.Errorf("error dropping collection: %v", err)
			}
		}
	}

	var collectionOptions *mgo.CollectionInfo

	//first create collection with options
	if intent.MetadataPath != "" {
		jsonBytes, err := ioutil.ReadFile(intent.MetadataPath)
		if err != nil {
			return fmt.Errorf("error reading metadata file: %v", err) //TODO better errors here
		}
		collectionOptions, _, err = MetadataFromJSON(jsonBytes)
		if err != nil {
			return fmt.Errorf("error parsing metadata file: %v", err)
		}
		if collectionOptions != nil {
			if collectionExists {
				log.Logf(1, "collection %v already exists", intent.Key())
			} else {
				log.Logf(1, "creating collection %v using options from metadata", intent.Key())
				err = c.Create(collectionOptions)
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
		log.Logf(1, "file %v is %v bytes", intent.BSONPath, size)

		rawFile, err := os.Open(intent.BSONPath)
		if err != nil {
			return fmt.Errorf("error reading bson file: %v", err)
		}

		bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(rawFile))
		defer bsonSource.Close()

		return restore.RestoreCollectionToDB(intent.DB, intent.C, bsonSource, size)
	}
	return nil

	//finally, add indexes
}

func (restore *MongoRestore) RestoreCollectionToDB(dbName, colName string,
	bsonSource *db.DecodedBSONSource, fileSize int64) error {

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("can't esablish session: %v", err)
	}
	defer session.Close()

	session.SetSafe(restore.safety)
	c := session.DB(dbName).C(colName)

	if restore.safety == nil && !restore.OutputOptions.Drop {
		//TODO check if the collection already exists!
		log.Logf(0, "restoring to %v.%v without dropping", dbName, colName)
		log.Log(0, "IMPORTANT: restored data will be inserted without raising errors; check your server log")
	}

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
		//TODO objcheck
		err := c.Insert(doc)
		if err != nil {
			fmt.Println(err)
			break
		}
	}
	if err = bsonSource.Err(); err != nil {
		return err
	}
	return nil
}
