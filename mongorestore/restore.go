package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"gopkg.in/mgo.v2/bson"
	"io/ioutil"
	"os"
	"strings"
	"time"
)

const ProgressBarLength = 24

// RestoreIntents iterates through all of the normal intents
// stored in the IntentManager, and restores them.
func (restore *MongoRestore) RestoreIntents() error {

	if restore.OutputOptions.JobThreads > 0 {
		errChan := make(chan error)
		doneChan := make(chan struct{})
		doneCount := 0

		// start a goroutine for each job thread
		for i := 0; i < restore.OutputOptions.JobThreads; i++ {
			go func(id int) {
				log.Logf(3, "starting restore routine with id=%v", id)
				for {
					intent := restore.manager.Pop()
					if intent == nil {
						break
					}
					err := restore.RestoreIntent(intent)
					if err != nil {
						errChan <- err
						return
					}
					restore.manager.Finish(intent)
				}
				log.Logf(3, "ending restore routine with id=%v, no more work to do", id)
				doneChan <- struct{}{}
			}(i)
		}

		// wait until all goroutines are done or one of them errors out
		for {
			select {
			case err := <-errChan:
				return err
			case <-doneChan:
				doneCount++
				if doneCount == restore.OutputOptions.JobThreads {
					return nil
				}
			}
		}
	}

	// single-threaded
	for intent := restore.manager.Pop(); intent != nil; intent = restore.manager.Pop() {
		err := restore.RestoreIntent(intent)
		if err != nil {
			return err
		}
		restore.manager.Finish(intent)
	}
	return nil
}

// RestoreIntent does the bulk of the logic to restore a collection
// from the BSON and metadata files linked to in the given intent.
// TODO: overly didactic comments on each step
func (restore *MongoRestore) RestoreIntent(intent *Intent) error {

	collectionExists, err := restore.DBHasCollection(intent)
	if err != nil {
		return fmt.Errorf("error reading database: %v", err)
	}

	if restore.safety == nil && !restore.OutputOptions.Drop && collectionExists {
		log.Logf(0, "restoring to existing collection %v without dropping", intent.Key())
		log.Log(0, "IMPORTANT: restored data will be inserted without raising errors; check your server log")
	}

	if restore.OutputOptions.Drop {
		if collectionExists {
			if strings.HasPrefix(intent.C, "system.") {
				log.Logf(0, "cannot drop system collection %v, skipping", intent.Key())
			} else {
				log.Logf(1, "dropping collection %v before restoring", intent.Key())
				// TODO(erf) maybe encapsulate this so that the session is closed sooner
				session, err := restore.SessionProvider.GetSession()
				session.SetSocketTimeout(0)
				if err != nil {
					return fmt.Errorf("error establishing connection: %v", err)
				}
				defer session.Close()
				err = session.DB(intent.DB).C(intent.C).DropCollection()
				if err != nil {
					return fmt.Errorf("error dropping collection: %v", err)
				}
				collectionExists = false
			}
		} else {
			log.Logf(2, "collection %v doesn't exist, skipping drop command", intent.Key())
		}
	}

	var options bson.D
	var indexes []IndexDocument

	// get indexes from system.indexes dump if we have it but don't have metadata files
	if intent.MetadataPath == "" && restore.manager.SystemIndexes(intent.DB) != nil {
		systemIndexesFile := restore.manager.SystemIndexes(intent.DB).BSONPath
		log.Logf(0, "no metadata file; reading indexes from %v", systemIndexesFile)
		indexes, err = restore.IndexesFromBSON(intent, systemIndexesFile)
		if err != nil {
			return fmt.Errorf("error reading indexes: %v", err)
		}
	}

	// first create collection with options
	if intent.MetadataPath != "" {
		log.Logf(0, "reading metadata file from %v", intent.MetadataPath)
		jsonBytes, err := ioutil.ReadFile(intent.MetadataPath)
		if err != nil {
			return fmt.Errorf("error reading metadata file: %v", err) //TODO better errors here
		}
		options, indexes, err = restore.MetadataFromJSON(jsonBytes)
		if err != nil {
			return fmt.Errorf("error parsing metadata file (%v): %v", string(jsonBytes), err)
		}
		if !restore.OutputOptions.NoOptionsRestore {
			if options != nil {
				if !collectionExists {
					log.Logf(1, "creating collection %v using options from metadata", intent.Key())
					err = restore.CreateCollection(intent, options)
					if err != nil {
						return fmt.Errorf("error creating collection %v: %v", intent.Key(), err)
					}
				} else {
					log.Logf(1, "collection %v already exists", intent.Key())
				}
			} else {
				log.Log(1, "no collection options to restore")
			}
		} else {
			log.Log(1, "skipping options restoration")
		}
	}

	// then do bson
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

	// finally, add indexes
	if len(indexes) > 0 && !restore.OutputOptions.NoIndexRestore {
		log.Logf(0, "restoring indexes for collection %v from metadata", intent.Key())
		//TODO better logging
		err = restore.CreateIndexes(intent, indexes)
		if err != nil {
			return fmt.Errorf("error creating indexes: %v", err)
		}
	} else {
		log.Log(0, "no indexes to restore")
	}
	return nil
}

// RestoreCollectionToDB pipes the given BSON data into the database.
func (restore *MongoRestore) RestoreCollectionToDB(dbName, colName string,
	bsonSource *db.DecodedBSONSource, fileSize int64) error {

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error establishing connection: %v", err)
	}
	session.SetSafe(restore.safety)
	session.SetSocketTimeout(0)
	defer session.Close()

	collection := session.DB(dbName).C(colName)

	// progress bar handlers
	bytesRead := 0

	bar := progress.ProgressBar{
		Name:       fmt.Sprintf("%v.%v", dbName, colName),
		Max:        int(fileSize),
		CounterPtr: &bytesRead,
		WaitTime:   3 * time.Second,
		Writer:     log.Writer(0),
		BarLength:  ProgressBarLength,
	}
	bar.Start()
	defer bar.Stop()

	MaxInsertThreads := restore.OutputOptions.BulkWriters
	if restore.OutputOptions.PreserveDocOrder {
		MaxInsertThreads = 1
	}
	if MaxInsertThreads == 0 {
		MaxInsertThreads = 4 //default
	}
	BulkBufferSize := restore.OutputOptions.BulkBufferSize
	if BulkBufferSize == 0 {
		BulkBufferSize = 2048
	}
	docChan := make(chan bson.Raw, BulkBufferSize*MaxInsertThreads)
	resultChan := make(chan error, MaxInsertThreads)
	killChan := make(chan struct{})

	// start a goroutine for adding up the number of bytes read
	bytesReadChan := make(chan int, MaxInsertThreads*16)
	go func() {
		for {
			size, alive := <-bytesReadChan
			if !alive {
				return
			}
			bytesRead += size
		}
	}()
	defer close(bytesReadChan)

	go func() {
		doc := bson.Raw{}
		for bsonSource.Next(&doc) {
			rawBytes := make([]byte, len(doc.Data))
			copy(rawBytes, doc.Data)
			docChan <- bson.Raw{Data: rawBytes}
		}
		close(docChan)
	}()

	for i := 0; i < MaxInsertThreads; i++ {
		go func() {
			bulk := db.NewBufferedBulkInserter(collection, BulkBufferSize, false)
			for {
				select {
				case rawDoc, alive := <-docChan:
					if !alive {
						resultChan <- bulk.Flush()
						return
					}
					if restore.objCheck {
						//TODO encapsulate to reuse bson obj??
						err := bson.Unmarshal(rawDoc.Data, &bson.D{})
						if err != nil {
							resultChan <- fmt.Errorf("invalid object: %v", err)
							return
						}
					}
					err := bulk.Insert(rawDoc)
					if err != nil {
						resultChan <- err
						return
					}
					bytesReadChan <- len(rawDoc.Data)
				case <-killChan:
					return
				}
			}
		}()

		// sleep to prevent all threads from inserting at the same time at start
		time.Sleep(time.Duration(i) * 10 * time.Millisecond) //FIXME magic numbers
	}

	// wait until all insert jobs finish
	for done := 0; done < MaxInsertThreads; done++ {
		err := <-resultChan
		if err != nil {
			close(killChan)
			//TODO actually wait for things to end?
			return err
		}
	}
	// final error check
	if err = bsonSource.Err(); err != nil {
		return err
	}
	return nil
}
