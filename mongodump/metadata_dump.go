package mongodump

import (
	"bufio"
	"encoding/json"
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2/bson"
	"io"
)

// Metadata consists of two parts, a collection's options and
// its indexes. Options include properties like "capped" etc.
// Metadata JSON is read in by mongorestore to properly recreate
// collections with the proper options and indexes.
type Metadata struct {
	Options bson.M          `json:"options,omitempty"`
	Indexes []IndexDocument `json:"indexes"`
}

// IndexDocument is used to write out the index info as json
type IndexDocument bson.M

// IndexDocumentFromDB is used internally to preserve key ordering
type IndexDocumentFromDB struct {
	Options bson.M `bson:",inline"`
	Key     bson.D `bson:"key"`
}

// This helper gets the metadata for a collection and writes it
// in readable JSON format
func (dump *MongoDump) dumpMetadataToWriter(dbName, c string, writer io.Writer) error {
	// make a buffered writer for nicer disk i/o
	w := bufio.NewWriter(writer)

	nsID := fmt.Sprintf("%v.%v", dbName, c)
	meta := Metadata{
		// We have to initialize Indexes to an empty slice, not nil, so that an empty
		// array is marshalled into json instead of null. That is, {indexes:[]} is okay
		// but {indexes:null} will cause assertions in our legacy C++ mongotools
		Indexes: []IndexDocument{},
	}

	// First, we get the options for the collection. These are pulled
	// using either listCollections (2.7+) or by querying system.namespaces
	// (2.6 and earlier), the internal collection containing collection names
	// and properties. For mongodump, we copy just the "options"
	// subdocument for the collection.
	log.Logf(log.DebugHigh, "\treading options for `%v`", nsID)

	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()
	collection := session.DB(dbName).C(c)

	collectionInfo, err := db.GetCollectionOptions(collection)
	if collectionInfo == nil {
		//The collection wasn't found, which means it was probably deleted
		// between now and the time that collections were listed. Skip it.
		log.Logf(log.DebugLow, "Warning: no metadata found for collection: `%v`: %v", nsID, err)
		return nil
	}
	meta.Options = bson.M{}
	if opts, err := bsonutil.FindValueByKey("options", collectionInfo); err == nil {
		if optsD, ok := opts.(bson.D); ok {
			meta.Options = optsD.Map()
		} else {
			return fmt.Errorf("Collection options contains invalid data: %v", opts)
		}
	}

	// Second, we read the collection's index information by either calling
	// listIndexes (pre-2.7 systems) or querying system.indexes.
	// We keep a running list of all the indexes
	// for the current collection as we iterate over the cursor, and include
	// that list as the "indexes" field of the metadata document.
	log.Logf(log.DebugHigh, "\treading indexes for `%v`", nsID)

	//get the indexes
	indexes, err := db.GetIndexes(collection)
	if err != nil {
		return err
	}
	for _, index := range indexes {
		marshalableIndex := IndexDocument{}
		for _, indexDocElem := range index {
			if indexDocElem.Name == "key" {
				if indexAsBsonD, ok := indexDocElem.Value.(bson.D); ok {
					marshalableIndex["key"] = bsonutil.MarshalD(indexAsBsonD)
				} else {
					return fmt.Errorf("index key could not be found in: %v", indexDocElem.Value)
				}
			} else {
				marshalableIndex[indexDocElem.Name] = indexDocElem.Value
			}
		}
		meta.Indexes = append(meta.Indexes, marshalableIndex)
	}

	// Finally, we send the results to the writer as JSON bytes
	jsonBytes, err := json.Marshal(meta)
	if err != nil {
		return fmt.Errorf("error marshalling metadata json for collection `%v`: %v", nsID, err)
	}
	_, err = w.Write(jsonBytes)
	if err != nil {
		return fmt.Errorf("error writing metadata for collection `%v` to disk: %v", nsID, err)
	}
	err = w.Flush()
	if err != nil {
		return fmt.Errorf("error writing metadata for collection `%v` to disk: %v", nsID, err)
	}
	return nil
}
