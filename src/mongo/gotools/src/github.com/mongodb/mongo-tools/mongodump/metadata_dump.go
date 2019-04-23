// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongodump

import (
	"context"
	"fmt"
	"io"

	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/intents"
	"github.com/mongodb/mongo-tools-common/log"
	"go.mongodb.org/mongo-driver/bson"
)

// Metadata holds information about a collection's options and indexes.
type Metadata struct {
	Options bson.M   `json:"options,omitempty"`
	Indexes []bson.D `json:"indexes"`
	UUID    string   `json:"uuid,omitempty"`
}

// IndexDocumentFromDB is used internally to preserve key ordering.
type IndexDocumentFromDB struct {
	Options bson.M `bson:",inline"`
	Key     bson.D `bson:"key"`
}

// dumpMetadata gets the metadata for a collection and writes it
// in readable JSON format.
func (dump *MongoDump) dumpMetadata(intent *intents.Intent, buffer resettableOutputBuffer) (err error) {

	meta := Metadata{
		// We have to initialize Indexes to an empty slice, not nil, so that an empty
		// array is marshalled into json instead of null. That is, {indexes:[]} is okay
		// but {indexes:null} will cause assertions in our legacy C++ mongotools
		Indexes: []bson.D{},
	}

	// The collection options were already gathered while building the list of intents.
	meta.Options = intent.Options

	// If a collection has a UUID, it was gathered while building the list of
	// intents.  Otherwise, it will be the empty string.
	meta.UUID = intent.UUID

	// Second, we read the collection's index information by either calling
	// listIndexes (pre-2.7 systems) or querying system.indexes.
	// We keep a running list of all the indexes
	// for the current collection as we iterate over the cursor, and include
	// that list as the "indexes" field of the metadata document.
	log.Logvf(log.DebugHigh, "\treading indexes for `%v`", intent.Namespace())

	session, err := dump.SessionProvider.GetSession()
	if err != nil {
		return err
	}

	if intent.IsView() {
		log.Logvf(log.DebugLow, "not dumping indexes metadata for '%v' because it is a view", intent.Namespace())
	} else {
		// get the indexes
		indexesIter, err := db.GetIndexes(session.Database(intent.DB).Collection(intent.C))
		defer indexesIter.Close(context.Background())
		if err != nil {
			return err
		}
		if indexesIter == nil {
			log.Logvf(log.Always, "the collection %v appears to have been dropped after the dump started", intent.Namespace())
			return nil
		}

		ctx := context.Background()
		for indexesIter.Next(ctx) {
			indexOpts := &bson.D{}
			err := indexesIter.Decode(indexOpts)
			if err != nil {
				return fmt.Errorf("error converting index: %v", err)
			}

			meta.Indexes = append(meta.Indexes, *indexOpts)
		}

		if err := indexesIter.Err(); err != nil {
			return fmt.Errorf("error getting indexes for collection `%v`: %v", intent.Namespace(), err)
		}
	}

	// Finally, we send the results to the writer as JSON bytes
	jsonBytes, err := bson.MarshalExtJSON(meta, true, false)
	if err != nil {
		return fmt.Errorf("error marshalling metadata json for collection `%v`: %v", intent.Namespace(), err)
	}

	err = intent.MetadataFile.Open()
	if err != nil {
		return err
	}
	defer func() {
		closeErr := intent.MetadataFile.Close()
		if err == nil && closeErr != nil {
			err = fmt.Errorf("error writing metadata for collection `%v` to disk: %v", intent.Namespace(), closeErr)
		}
	}()

	var f io.Writer
	f = intent.MetadataFile
	if buffer != nil {
		buffer.Reset(f)
		f = buffer
		defer func() {
			closeErr := buffer.Close()
			if err == nil && closeErr != nil {
				err = fmt.Errorf("error writing metadata for collection `%v` to disk: %v", intent.Namespace(), closeErr)
			}
		}()
	}
	_, err = f.Write(jsonBytes)
	if err != nil {
		err = fmt.Errorf("error writing metadata for collection `%v` to disk: %v", intent.Namespace(), err)
	}
	return
}
