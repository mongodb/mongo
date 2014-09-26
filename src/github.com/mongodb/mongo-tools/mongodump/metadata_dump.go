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
	"os"
)

// Metadata consists of two parts, a collection's options and
// its indexes. Options include properties like "capped" etc.
// Metadata JSON is read in by mongorestore to properly recreate
// collections with the proper options and indexes.
type Metadata struct {
	Options bson.M     `json:"options,omitempty"`
	Indexes []IndexDoc `json:"indexes"`
}

// We need two separate types here, because we cannot read BSON
// into our custom MarshalD type, but we cannot create JSON
// using mgo's bson.D type. This should be fixed with a driver
// change before release... TODO
type IndexDoc struct {
	Name      string            `json:"name"`
	Namespace string            `json:"ns"`
	Value     int               `json:"v"`
	Key       bsonutil.MarshalD `json:"key"`
}

type IndexDocFromDB struct {
	Name      string `bson:"name"`
	Namespace string `bson:"ns"`
	Value     int    `bson:"v"`
	Key       bson.D `bson:"key"`
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
		Indexes: []IndexDoc{},
	}

	// First, we get the options for the collection. These are pulled
	// from the system.namespaces collection, which is the hidden internal
	// collection for tracking collection names and properties. For mongodump,
	// we copy just the "options" subdocument for the collection.
	log.Logf(3, "\treading options for `%v`", nsID)
	namespaceDoc := bson.M{}
	err := dump.cmdRunner.FindOne(dbName, "system.namespaces", 0, bson.M{"name": nsID}, nil, namespaceDoc, 0)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Warning: no metadata found for collection: `%v`: %v", nsID, err)
		return nil
	}
	if opts, ok := namespaceDoc["options"]; ok {
		meta.Options = opts.(bson.M)
	}

	// Second, we read the collection's index information from the
	// system.indexes collection. We keep a running list of all the indexes
	// for the current collection as we iterate over the cursor, and include
	// that list as the "indexes" field of the metadata document.
	log.Logf(3, "\treading indexes for `%v`", nsID)

	cursor, err := dump.cmdRunner.FindDocs(dbName, "system.indexes", 0, 0, bson.M{"ns": nsID}, nil, db.Snapshot)
	if err != nil {
		return err
	}
	defer cursor.Close()
	indexDocDB := IndexDocFromDB{}
	for cursor.Next(&indexDocDB) {
		// FIXME this is super annoying and could be fixed with an mgo pull request
		indexDoc := IndexDoc{
			Name:      indexDocDB.Name,
			Namespace: indexDocDB.Namespace,
			Value:     indexDocDB.Value,
			Key:       bsonutil.MarshalD(indexDocDB.Key),
		}
		meta.Indexes = append(meta.Indexes, indexDoc)
	}
	if err := cursor.Err(); err != nil {
		return fmt.Errorf("error finding index data for collection `%v`: %v", nsID, err)
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
