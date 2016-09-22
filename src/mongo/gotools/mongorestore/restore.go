package mongorestore

import (
	"fmt"
	"io/ioutil"
	"strings"
	"time"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
)

const insertBufferFactor = 16

// RestoreIntents iterates through all of the intents stored in the IntentManager, and restores them.
func (restore *MongoRestore) RestoreIntents() error {
	log.Logvf(log.DebugLow, "restoring up to %v collections in parallel", restore.OutputOptions.NumParallelCollections)

	if restore.OutputOptions.NumParallelCollections > 0 {
		resultChan := make(chan error)

		// start a goroutine for each job thread
		for i := 0; i < restore.OutputOptions.NumParallelCollections; i++ {
			go func(id int) {
				log.Logvf(log.DebugHigh, "starting restore routine with id=%v", id)
				var ioBuf []byte
				for {
					intent := restore.manager.Pop()
					if intent == nil {
						log.Logvf(log.DebugHigh, "ending restore routine with id=%v, no more work to do", id)
						resultChan <- nil // done
						return
					}
					if fileNeedsIOBuffer, ok := intent.BSONFile.(intents.FileNeedsIOBuffer); ok {
						if ioBuf == nil {
							ioBuf = make([]byte, db.MaxBSONSize)
						}
						fileNeedsIOBuffer.TakeIOBuffer(ioBuf)
					}
					err := restore.RestoreIntent(intent)
					if err != nil {
						resultChan <- fmt.Errorf("%v: %v", intent.Namespace(), err)
						return
					}
					restore.manager.Finish(intent)
					if fileNeedsIOBuffer, ok := intent.BSONFile.(intents.FileNeedsIOBuffer); ok {
						fileNeedsIOBuffer.ReleaseIOBuffer()
					}

				}
			}(i)
		}

		// wait until all goroutines are done or one of them errors out
		for i := 0; i < restore.OutputOptions.NumParallelCollections; i++ {
			if err := <-resultChan; err != nil {
				return err
			}
		}
		return nil
	}

	// single-threaded
	for {
		intent := restore.manager.Pop()
		if intent == nil {
			break
		}
		err := restore.RestoreIntent(intent)
		if err != nil {
			return fmt.Errorf("%v: %v", intent.Namespace(), err)
		}
		restore.manager.Finish(intent)
	}
	return nil
}

// RestoreIntent attempts to restore a given intent into MongoDB.
func (restore *MongoRestore) RestoreIntent(intent *intents.Intent) error {

	collectionExists, err := restore.CollectionExists(intent)
	if err != nil {
		return fmt.Errorf("error reading database: %v", err)
	}

	if restore.safety == nil && !restore.OutputOptions.Drop && collectionExists {
		log.Logvf(log.Always, "restoring to existing collection %v without dropping", intent.Namespace())
		log.Logv(log.Always, "Important: restored data will be inserted without raising errors; check your server log")
	}

	if restore.OutputOptions.Drop {
		if collectionExists {
			if strings.HasPrefix(intent.C, "system.") {
				log.Logvf(log.Always, "cannot drop system collection %v, skipping", intent.Namespace())
			} else {
				log.Logvf(log.Info, "dropping collection %v before restoring", intent.Namespace())
				err = restore.DropCollection(intent)
				if err != nil {
					return err // no context needed
				}
				collectionExists = false
			}
		} else {
			log.Logvf(log.DebugLow, "collection %v doesn't exist, skipping drop command", intent.Namespace())
		}
	}

	var options bson.D
	var indexes []IndexDocument

	// get indexes from system.indexes dump if we have it but don't have metadata files
	if intent.MetadataFile == nil {
		if _, ok := restore.dbCollectionIndexes[intent.DB]; ok {
			if indexes, ok = restore.dbCollectionIndexes[intent.DB][intent.C]; ok {
				log.Logvf(log.Always, "no metadata; falling back to system.indexes")
			}
		}
	}

	logMessageSuffix := "with no metadata"
	// first create the collection with options from the metadata file
	if intent.MetadataFile != nil {
		logMessageSuffix = "using options from metadata"
		err = intent.MetadataFile.Open()
		if err != nil {
			return err
		}
		defer intent.MetadataFile.Close()

		log.Logvf(log.Always, "reading metadata for %v from %v", intent.Namespace(), intent.MetadataLocation)
		metadata, err := ioutil.ReadAll(intent.MetadataFile)
		if err != nil {
			return fmt.Errorf("error reading metadata from %v: %v", intent.MetadataLocation, err)
		}
		options, indexes, err = restore.MetadataFromJSON(metadata)
		if err != nil {
			return fmt.Errorf("error parsing metadata from %v: %v", intent.MetadataLocation, err)
		}

		if restore.OutputOptions.NoOptionsRestore {
			log.Logv(log.Info, "not restoring collection options")
			logMessageSuffix = "with no collection options"
			options = nil
		}
	}
	if !collectionExists {
		log.Logvf(log.Info, "creating collection %v %s", intent.Namespace(), logMessageSuffix)
		log.Logvf(log.DebugHigh, "using collection options: %#v", options)
		err = restore.CreateCollection(intent, options)
		if err != nil {
			return fmt.Errorf("error creating collection %v: %v", intent.Namespace(), err)
		}
	} else {
		log.Logvf(log.Info, "collection %v already exists - skipping collection create", intent.Namespace())
	}

	var documentCount int64
	if intent.BSONFile != nil {
		err = intent.BSONFile.Open()
		if err != nil {
			return err
		}
		defer intent.BSONFile.Close()

		log.Logvf(log.Always, "restoring %v from %v", intent.Namespace(), intent.Location)

		bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(intent.BSONFile))
		defer bsonSource.Close()

		documentCount, err = restore.RestoreCollectionToDB(intent.DB, intent.C, bsonSource, intent.BSONFile, intent.Size)
		if err != nil {
			return fmt.Errorf("error restoring from %v: %v", intent.Location, err)
		}
	}

	// finally, add indexes
	if len(indexes) > 0 && !restore.OutputOptions.NoIndexRestore {
		log.Logvf(log.Always, "restoring indexes for collection %v from metadata", intent.Namespace())
		err = restore.CreateIndexes(intent, indexes)
		if err != nil {
			return fmt.Errorf("error creating indexes for %v: %v", intent.Namespace(), err)
		}
	} else {
		log.Logv(log.Always, "no indexes to restore")
	}

	log.Logvf(log.Always, "finished restoring %v (%v %v)",
		intent.Namespace(), documentCount, util.Pluralize(int(documentCount), "document", "documents"))
	return nil
}

// RestoreCollectionToDB pipes the given BSON data into the database.
// Returns the number of documents restored and any errors that occured.
func (restore *MongoRestore) RestoreCollectionToDB(dbName, colName string,
	bsonSource *db.DecodedBSONSource, file PosReader, fileSize int64) (int64, error) {

	var termErr error
	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return int64(0), fmt.Errorf("error establishing connection: %v", err)
	}
	session.SetSafe(restore.safety)
	defer session.Close()

	collection := session.DB(dbName).C(colName)

	documentCount := int64(0)
	watchProgressor := progress.NewCounter(fileSize)
	if restore.ProgressManager != nil {
		name := fmt.Sprintf("%v.%v", dbName, colName)
		restore.ProgressManager.Attach(name, watchProgressor)
		defer restore.ProgressManager.Detach(name)
	}

	maxInsertWorkers := restore.OutputOptions.NumInsertionWorkers
	if restore.OutputOptions.MaintainInsertionOrder {
		maxInsertWorkers = 1
	}

	docChan := make(chan bson.Raw, insertBufferFactor)
	resultChan := make(chan error, maxInsertWorkers)

	// stream documents for this collection on docChan
	go func() {
		doc := bson.Raw{}
		for bsonSource.Next(&doc) {
			select {
			case <-restore.termChan:
				log.Logvf(log.Always, "terminating read on %v.%v", dbName, colName)
				termErr = util.ErrTerminated
				close(docChan)
				return
			default:
				rawBytes := make([]byte, len(doc.Data))
				copy(rawBytes, doc.Data)
				docChan <- bson.Raw{Data: rawBytes}
				documentCount++
			}
		}
		close(docChan)
	}()

	log.Logvf(log.DebugLow, "using %v insertion workers", maxInsertWorkers)

	for i := 0; i < maxInsertWorkers; i++ {
		go func() {
			// get a session copy for each insert worker
			s := session.Copy()
			defer s.Close()

			coll := collection.With(s)
			bulk := db.NewBufferedBulkInserter(
				coll, restore.OutputOptions.BulkBufferSize, !restore.OutputOptions.StopOnError)
			for rawDoc := range docChan {
				if restore.objCheck {
					err := bson.Unmarshal(rawDoc.Data, &bson.D{})
					if err != nil {
						resultChan <- fmt.Errorf("invalid object: %v", err)
						return
					}
				}
				if err := bulk.Insert(rawDoc); err != nil {
					if db.IsConnectionError(err) || restore.OutputOptions.StopOnError {
						// Propagate this error, since it's either a fatal connection error
						// or the user has turned on --stopOnError
						resultChan <- err
					} else {
						// Otherwise just log the error but don't propagate it.
						log.Logvf(log.Always, "error: %v", err)
					}
				}
				watchProgressor.Set(file.Pos())
			}
			err := bulk.Flush()
			if err != nil {
				if !db.IsConnectionError(err) && !restore.OutputOptions.StopOnError {
					// Suppress this error since it's not a severe connection error and
					// the user has not specified --stopOnError
					log.Logvf(log.Always, "error: %v", err)
					err = nil
				}
			}
			resultChan <- err
			return
		}()

		// sleep to prevent all threads from inserting at the same time at start
		time.Sleep(time.Duration(i) * 10 * time.Millisecond)
	}

	// wait until all insert jobs finish
	for done := 0; done < maxInsertWorkers; done++ {
		err := <-resultChan
		if err != nil {
			return int64(0), fmt.Errorf("insertion error: %v", err)
		}
	}

	// final error check
	if err = bsonSource.Err(); err != nil {
		return int64(0), fmt.Errorf("reading bson input: %v", err)
	}
	return documentCount, termErr
}
