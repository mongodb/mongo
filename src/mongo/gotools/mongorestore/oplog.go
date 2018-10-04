// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"fmt"
	"strconv"
	"strings"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

// oplogMaxCommandSize sets the maximum size for multiple buffered ops in the
// applyOps command. This is to prevent pathological cases where the array overhead
// of many small operations can overflow the maximum command size.
// Note that ops > 8MB will still be buffered, just as single elements.
const oplogMaxCommandSize = 1024 * 1024 * 8

// RestoreOplog attempts to restore a MongoDB oplog.
func (restore *MongoRestore) RestoreOplog() error {
	log.Logv(log.Always, "replaying oplog")
	intent := restore.manager.Oplog()
	if intent == nil {
		// this should not be reached
		log.Logv(log.Always, "no oplog file provided, skipping oplog application")
		return nil
	}
	if err := intent.BSONFile.Open(); err != nil {
		return err
	}
	if fileNeedsIOBuffer, ok := intent.BSONFile.(intents.FileNeedsIOBuffer); ok {
		fileNeedsIOBuffer.TakeIOBuffer(make([]byte, db.MaxBSONSize))
	}
	defer intent.BSONFile.Close()
	// NewBufferlessBSONSource reads each bson document into its own buffer
	// because bson.Unmarshal currently can't unmarshal binary types without
	// them referencing the source buffer
	bsonSource := db.NewDecodedBSONSource(db.NewBufferlessBSONSource(intent.BSONFile))
	defer bsonSource.Close()

	rawOplogEntry := &bson.Raw{}

	var totalOps int64
	var entrySize int

	oplogProgressor := progress.NewCounter(intent.BSONSize)
	if restore.ProgressManager != nil {
		restore.ProgressManager.Attach("oplog", oplogProgressor)
		defer restore.ProgressManager.Detach("oplog")
	}

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error establishing connection: %v", err)
	}
	defer session.Close()

	for bsonSource.Next(rawOplogEntry) {
		entrySize = len(rawOplogEntry.Data)

		entryAsOplog := db.Oplog{}
		err = bson.Unmarshal(rawOplogEntry.Data, &entryAsOplog)
		if err != nil {
			return fmt.Errorf("error reading oplog: %v", err)
		}
		if entryAsOplog.Operation == "n" {
			//skip no-ops
			continue
		}
		if !restore.TimestampBeforeLimit(entryAsOplog.Timestamp) {
			log.Logvf(
				log.DebugLow,
				"timestamp %v is not below limit of %v; ending oplog restoration",
				entryAsOplog.Timestamp,
				restore.oplogLimit,
			)
			break
		}

		// TODO: TOOLS-1817 will add support for conditionally keeping UUIDS
		entryAsOplog, err = filterUUIDs(entryAsOplog)
		if err != nil {
			return fmt.Errorf("error filtering UUIDs from oplog: %v", err)
		}

		totalOps++
		oplogProgressor.Inc(int64(entrySize))
		err = restore.ApplyOps(session, []interface{}{entryAsOplog})
		if err != nil {
			return fmt.Errorf("error applying oplog: %v", err)
		}
	}
	if fileNeedsIOBuffer, ok := intent.BSONFile.(intents.FileNeedsIOBuffer); ok {
		fileNeedsIOBuffer.ReleaseIOBuffer()
	}

	log.Logvf(log.Info, "applied %v ops", totalOps)
	if err := bsonSource.Err(); err != nil {
		return fmt.Errorf("error reading oplog bson input: %v", err)
	}
	return nil

}

// ApplyOps is a wrapper for the applyOps database command, we pass in
// a session to avoid opening a new connection for a few inserts at a time.
func (restore *MongoRestore) ApplyOps(session *mgo.Session, entries []interface{}) error {
	res := bson.M{}
	err := session.Run(bson.D{{"applyOps", entries}}, &res)
	if err != nil {
		return fmt.Errorf("applyOps: %v", err)
	}
	if util.IsFalsy(res["ok"]) {
		return fmt.Errorf("applyOps command: %v", res["errmsg"])
	}

	return nil
}

// TimestampBeforeLimit returns true if the given timestamp is allowed to be
// applied to mongorestore's target database.
func (restore *MongoRestore) TimestampBeforeLimit(ts bson.MongoTimestamp) bool {
	if restore.oplogLimit == 0 {
		// always valid if there is no --oplogLimit set
		return true
	}
	return ts < restore.oplogLimit
}

// ParseTimestampFlag takes in a string the form of <time_t>:<ordinal>,
// where <time_t> is the seconds since the UNIX epoch, and <ordinal> represents
// a counter of operations in the oplog that occurred in the specified second.
// It parses this timestamp string and returns a bson.MongoTimestamp type.
func ParseTimestampFlag(ts string) (bson.MongoTimestamp, error) {
	var seconds, increment int
	timestampFields := strings.Split(ts, ":")
	if len(timestampFields) > 2 {
		return 0, fmt.Errorf("too many : characters")
	}

	seconds, err := strconv.Atoi(timestampFields[0])
	if err != nil {
		return 0, fmt.Errorf("error parsing timestamp seconds: %v", err)
	}

	// parse the increment field if it exists
	if len(timestampFields) == 2 {
		if len(timestampFields[1]) > 0 {
			increment, err = strconv.Atoi(timestampFields[1])
			if err != nil {
				return 0, fmt.Errorf("error parsing timestamp increment: %v", err)
			}
		} else {
			// handle the case where the user writes "<time_t>:" with no ordinal
			increment = 0
		}
	}

	timestamp := (int64(seconds) << 32) | int64(increment)
	return bson.MongoTimestamp(timestamp), nil
}

// filterUUIDs removes 'ui' entries from ops, including nested applyOps ops.
func filterUUIDs(op db.Oplog) (db.Oplog, error) {
	// Remove UUIDs from oplog entries
	if op.UI != nil {
		op.UI = nil
	}

	// Check for and filter nested applyOps ops
	if op.Operation == "c" && isApplyOpsCmd(op.Object) {
		filtered, err := newFilteredApplyOps(op.Object)
		if err != nil {
			return db.Oplog{}, err
		}
		op.Object = filtered
	}

	return op, nil
}

// isApplyOpsCmd returns true if a document seems to be an applyOps command.
func isApplyOpsCmd(cmd bson.RawD) bool {
	for _, v := range cmd {
		if v.Name == "applyOps" {
			return true
		}
	}
	return false
}

// newFilteredApplyOps iterates over nested ops in an applyOps document and
// returns a new applyOps document that omits the 'ui' field from nested ops.
func newFilteredApplyOps(cmd bson.RawD) (bson.RawD, error) {
	ops, err := unwrapNestedApplyOps(cmd)
	if err != nil {
		return nil, err
	}

	filtered := make([]db.Oplog, len(ops))
	for i, v := range ops {
		filtered[i], err = filterUUIDs(v)
		if err != nil {
			return nil, err
		}
	}

	doc, err := wrapNestedApplyOps(filtered)
	if err != nil {
		return nil, err
	}

	return doc, nil
}

// nestedApplyOps models an applyOps command document
type nestedApplyOps struct {
	ApplyOps []db.Oplog `bson:"applyOps"`
}

// unwrapNestedApplyOps converts a RawD to a typed data structure.
// Unfortunately, we're forced to convert by marshaling to bytes and
// unmarshaling.
func unwrapNestedApplyOps(doc bson.RawD) ([]db.Oplog, error) {
	// Doc to bytes
	bs, err := bson.Marshal(doc)
	if err != nil {
		return nil, fmt.Errorf("cannot remarshal nested applyOps: %s", err)
	}

	// Bytes to typed data
	var cmd nestedApplyOps
	err = bson.Unmarshal(bs, &cmd)
	if err != nil {
		return nil, fmt.Errorf("cannot unwrap nested applyOps: %s", err)
	}

	return cmd.ApplyOps, nil
}

// wrapNestedApplyOps converts a typed data structure to a RawD.
// Unfortunately, we're forced to convert by marshaling to bytes and
// unmarshaling.
func wrapNestedApplyOps(ops []db.Oplog) (bson.RawD, error) {
	cmd := &nestedApplyOps{ApplyOps: ops}

	// Typed data to bytes
	raw, err := bson.Marshal(cmd)
	if err != nil {
		return nil, fmt.Errorf("cannot rewrap nested applyOps op: %s", err)
	}

	// Bytes to doc
	var doc bson.RawD
	err = bson.Unmarshal(raw, &doc)
	if err != nil {
		return nil, fmt.Errorf("cannot reunmarshal nested applyOps op: %s", err)
	}

	return doc, nil
}
