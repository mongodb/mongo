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
	// from the system.namespaces collection, which is the hidden internal
	// collection for tracking collection names and properties. For mongodump,
	// we copy just the "options" subdocument for the collection.
	log.Logf(log.DebugHigh, "\treading options for `%v`", nsID)
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
	log.Logf(log.DebugHigh, "\treading indexes for `%v`", nsID)

	cursor, err := dump.cmdRunner.FindDocs(dbName, "system.indexes", 0, 0, bson.M{"ns": nsID}, nil, db.Snapshot)
	if err != nil {
		return err
	}
	defer cursor.Close()
	indexDoc := IndexDocumentFromDB{}
	for cursor.Next(&indexDoc) {
		// convert the IndexDocumentFromDB into a regular IndexDocument that we
		// can marshal into JSON.
		marshalableIndex := IndexDocument(indexDoc.Options)
		marshalableIndex["key"] = bsonutil.MarshalD(indexDoc.Key)
		meta.Indexes = append(meta.Indexes, marshalableIndex)
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
