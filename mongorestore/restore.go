package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"gopkg.in/mgo.v2/bson"
	"io"
	"io/ioutil"
	"os"
	"strings"
	"time"
)

const (
	ProgressBarLength   = 24
	ProgressBarWaitTime = time.Second * 3

	InsertBufferFactor = 16
)

// RestoreIntents iterates through all of the normal intents
// stored in the IntentManager, and restores them.
func (restore *MongoRestore) RestoreIntents() error {

	// start up the progress bar manager
	restore.progressManager = progress.NewProgressBarManager(ProgressBarWaitTime)
	restore.progressManager.Start()
	defer restore.progressManager.Stop()

	if restore.OutputOptions.NumParallelCollections > 0 {
		resultChan := make(chan error)

		// start a goroutine for each job thread
		for i := 0; i < restore.OutputOptions.NumParallelCollections; i++ {
			go func(id int) {
				log.Logf(log.DebugHigh, "starting restore routine with id=%v", id)
				for {
					intent := restore.manager.Pop()
					if intent == nil {
						break
					}
					err := restore.RestoreIntent(intent)
					if err != nil {
						resultChan <- fmt.Errorf("%v: %v", intent.Key(), err)
						return
					}
					restore.manager.Finish(intent)
				}
				log.Logf(log.DebugHigh, "ending restore routine with id=%v, no more work to do", id)
				resultChan <- nil // done
			}(i)
		}

		// wait until all goroutines are done or one of them errors out
		for i := 0; i < restore.OutputOptions.NumParallelCollections; i++ {
			select {
			case err := <-resultChan:
				if err != nil {
					return err
				}
			}
		}
		return nil
	}

	// single-threaded
	for intent := restore.manager.Pop(); intent != nil; intent = restore.manager.Pop() {
		err := restore.RestoreIntent(intent)
		if err != nil {
			return fmt.Errorf("%v: %v", intent.Key(), err)
		}
		restore.manager.Finish(intent)
	}
	return nil
}

// RestoreIntent does the bulk of the logic to restore a collection
// from the BSON and metadata files linked to in the given intent.
// TODO: overly didactic comments on each step
func (restore *MongoRestore) RestoreIntent(intent *intents.Intent) error {

	collectionExists, err := restore.CollectionExists(intent)
	if err != nil {
		return fmt.Errorf("error reading database: %v", err)
	}

	if restore.safety == nil && !restore.OutputOptions.Drop && collectionExists {
		log.Logf(log.Always, "restoring to existing collection %v without dropping", intent.Key())
		log.Log(log.Always, "IMPORTANT: restored data will be inserted without raising errors; check your server log")
	}

	if restore.OutputOptions.Drop {
		if collectionExists {
			if strings.HasPrefix(intent.C, "system.") {
				log.Logf(log.Always, "cannot drop system collection %v, skipping", intent.Key())
			} else {
				log.Logf(log.Info, "dropping collection %v before restoring", intent.Key())
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
			log.Logf(log.DebugLow, "collection %v doesn't exist, skipping drop command", intent.Key())
		}
	}

	var options bson.D
	var indexes []IndexDocument

	// get indexes from system.indexes dump if we have it but don't have metadata files
	if intent.MetadataPath == "" && restore.manager.SystemIndexes(intent.DB) != nil {
		systemIndexesFile := restore.manager.SystemIndexes(intent.DB).BSONPath
		log.Logf(log.Always, "no metadata file; reading indexes from %v", systemIndexesFile)
		indexes, err = restore.IndexesFromBSON(intent, systemIndexesFile)
		if err != nil {
			return fmt.Errorf("error reading indexes from %v: %v", systemIndexesFile, err)
		}
	}

	// first create collection with options
	if intent.MetadataPath != "" {
		log.Logf(log.Always, "reading metadata file from %v", intent.MetadataPath)
		jsonBytes, err := ioutil.ReadFile(intent.MetadataPath)
		if err != nil {
			return fmt.Errorf("error reading metadata file %v: %v", intent.MetadataPath, err)
		}
		options, indexes, err = restore.MetadataFromJSON(jsonBytes)
		if err != nil {
			return fmt.Errorf("error parsing metadata file %v: %v", intent.MetadataPath, err)
		}
		if !restore.OutputOptions.NoOptionsRestore {
			if options != nil {
				if !collectionExists {
					log.Logf(log.Info, "creating collection %v using options from metadata", intent.Key())
					err = restore.CreateCollection(intent, options)
					if err != nil {
						return fmt.Errorf("error creating collection %v: %v", intent.Key(), err)
					}
				} else {
					log.Logf(log.Info, "collection %v already exists", intent.Key())
				}
			} else {
				log.Log(log.Info, "no collection options to restore")
			}
		} else {
			log.Log(log.Info, "skipping options restoration")
		}
	}

	// then do bson
	if intent.BSONPath != "" {
		log.Logf(log.Always, "restoring %v from file %v", intent.Key(), intent.BSONPath)
		var rawBSONSource io.ReadCloser
		var size int64

		if restore.useStdin {
			rawBSONSource = os.Stdin
			log.Log(log.Always, "restoring from stdin")
		} else {
			fileInfo, err := os.Lstat(intent.BSONPath)
			if err != nil {
				return fmt.Errorf("error reading BSON file %v: %v", intent.BSONPath, err)
			}
			size = fileInfo.Size()
			log.Logf(log.Info, "\tfile %v is %v bytes", intent.BSONPath, size)

			rawBSONSource, err = os.Open(intent.BSONPath)
			if err != nil {
				return fmt.Errorf("error reading BSON file %v: %v", intent.BSONPath, err)
			}
		}

		bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(rawBSONSource))
		defer bsonSource.Close()

		err = restore.RestoreCollectionToDB(intent.DB, intent.C, bsonSource, size)
		if err != nil {
			return fmt.Errorf("error restoring from %v: %v", intent.BSONPath, err)
		}
	}

	// finally, add indexes
	if len(indexes) > 0 && !restore.OutputOptions.NoIndexRestore {
		log.Logf(log.Always, "restoring indexes for collection %v from metadata", intent.Key())
		err = restore.CreateIndexes(intent, indexes)
		if err != nil {
			return fmt.Errorf("error creating indexes for %v: %v", intent.Key(), err)
		}
	} else {
		log.Log(log.Always, "no indexes to restore")
	}

	log.Logf(log.Always, "finished restoring %v", intent.Key())
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
	var bytesRead int64

	// only print progress bar if we know the bounds
	// TODO have useful progress meters when max=0
	if fileSize > 0 {
		bar := &progress.ProgressBar{
			Name:       fmt.Sprintf("%v.%v", dbName, colName),
			Max:        int64(fileSize),
			CounterPtr: &bytesRead,
			Writer:     log.Writer(0),
			BarLength:  ProgressBarLength,
		}
		restore.progressManager.Attach(bar)
		defer restore.progressManager.Detach(bar)
	}

	MaxInsertThreads := restore.ToolOptions.BulkWriters
	if restore.OutputOptions.MaintainInsertionOrder {
		MaxInsertThreads = 1
	}
	docChan := make(chan bson.Raw, InsertBufferFactor)
	resultChan := make(chan error, MaxInsertThreads)
	killChan := make(chan struct{})
	// make sure goroutines clean up on error
	defer close(killChan)

	// start a goroutine for adding up the number of bytes read
	bytesReadChan := make(chan int64, InsertBufferFactor)
	go func() {
		for {
			select {
			case size, alive := <-bytesReadChan:
				if !alive {
					return
				}
				bytesRead += size
			case <-killChan:
				return
			}
		}
	}()

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
			bulk := db.NewBufferedBulkInserter(collection, restore.ToolOptions.BulkBufferSize, !restore.OutputOptions.StopOnError)
			for {
				select {
				case rawDoc, alive := <-docChan:
					if !alive {
						err := bulk.Flush()
						if err != nil {
							if !db.IsConnectionError(err) && !restore.OutputOptions.StopOnError {
								// Suppress this error since it's not a severe connection error and
								// the user has not specified --stopOnError
								log.Logf(log.Always, "error: %v", err)
								err = nil
							}
						}
						resultChan <- err
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

						if db.IsConnectionError(err) || restore.OutputOptions.StopOnError {
							// Propagate this error, since it's either a fatal connection error
							// or the user has turned on --stopOnError
							resultChan <- err
						} else {
							// Otherwise just log the error but don't propagate it.
							log.Logf(log.Always, "error: %v", err)
						}
					}
					bytesReadChan <- int64(len(rawDoc.Data))
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
			return err
		}
	}
	// final error check
	if err = bsonSource.Err(); err != nil {
		return err
	}
	return nil
}
