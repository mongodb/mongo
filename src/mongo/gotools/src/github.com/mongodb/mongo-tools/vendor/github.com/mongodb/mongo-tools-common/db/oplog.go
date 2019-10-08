package db

import (
	"context"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo"
	mopts "go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
)

// ApplyOpsResponse represents the response from an 'applyOps' command.
type ApplyOpsResponse struct {
	Ok     bool   `bson:"ok"`
	ErrMsg string `bson:"errmsg"`
}

// Oplog represents a MongoDB oplog document.
type Oplog struct {
	Timestamp  primitive.Timestamp `bson:"ts"`
	HistoryID  int64               `bson:"h"`
	Version    int                 `bson:"v"`
	Operation  string              `bson:"op"`
	Namespace  string              `bson:"ns"`
	Object     bson.D              `bson:"o"`
	Query      bson.D              `bson:"o2,omitempty"`
	UI         *primitive.Binary   `bson:"ui,omitempty"`
	LSID       bson.Raw            `bson:"lsid,omitempty"`
	TxnNumber  *int64              `bson:"txnNumber,omitempty"`
	PrevOpTime bson.Raw            `bson:"prevOpTime,omitempty"`
}

// OplogTailTime represents two ways of describing the "end" of the oplog at a
// point in time.  The Latest field represents the last visible (storage
// committed) timestamp.  The Restart field represents a (possibly older)
// timestamp that can be used to start tailing or copying the oplog without
// losing parts of transactions in progress.
type OplogTailTime struct {
	Latest  primitive.Timestamp
	Restart primitive.Timestamp
}

var zeroTimestamp = primitive.Timestamp{}

// GetOplogTailTime constructs an OplogTailTime
func GetOplogTailTime(client *mongo.Client) (OplogTailTime, error) {
	// Check oldest active first to be sure it is less-than-or-equal to the
	// latest visible.
	oldestActive, err := GetOldestActiveTransactionTimestamp(client)
	if err != nil {
		return OplogTailTime{}, err
	}
	latestVisible, err := GetLatestVisibleOplogTimestamp(client)
	if err != nil {
		return OplogTailTime{}, err
	}
	// No oldest active means the latest visible is the restart time as well.
	if oldestActive == zeroTimestamp {
		return OplogTailTime{Latest: latestVisible, Restart: latestVisible}, nil
	}
	return OplogTailTime{Latest: latestVisible, Restart: oldestActive}, nil
}

// GetOldestActiveTransactionTimestamp returns the oldest active transaction
// timestamp from the config.transactions table or else a zero-value
// primitive.Timestamp.
func GetOldestActiveTransactionTimestamp(client *mongo.Client) (primitive.Timestamp, error) {
	coll := client.Database("config").Collection("transactions", mopts.Collection().SetReadConcern(readconcern.Local()))
	filter := bson.D{{"state", bson.D{{"$in", bson.A{"prepared", "inProgress"}}}}}
	opts := mopts.FindOne().SetSort(bson.D{{"startOpTime", 1}})

	result, err := coll.FindOne(context.Background(), filter, opts).DecodeBytes()
	if err != nil {
		if err == mongo.ErrNoDocuments {
			return primitive.Timestamp{}, nil
		}
		return primitive.Timestamp{}, fmt.Errorf("config.transactions.findOne error: %v", err)
	}

	rawTS, err := result.LookupErr("startOpTime", "ts")
	if err != nil {
		return primitive.Timestamp{}, fmt.Errorf("config.transactions row had no startOpTime.ts field")
	}

	t, i, ok := rawTS.TimestampOK()
	if !ok {
		return primitive.Timestamp{}, fmt.Errorf("config.transactions startOpTime.ts was not a BSON timestamp")
	}

	return primitive.Timestamp{T: t, I: i}, nil
}

// GetLatestVisibleOplogTimestamp returns the timestamp of the most recent
// "visible" oplog record. By "visible", we mean that all prior oplog entries
// have been storage-committed. See SERVER-30724 for a more detailed
// description.
func GetLatestVisibleOplogTimestamp(client *mongo.Client) (primitive.Timestamp, error) {
	latestOpTime, err := GetLatestOplogTimestamp(client, bson.D{})
	if err != nil {
		return primitive.Timestamp{}, err
	}
	// Do a forward scan starting at the last op fetched to ensure that
	// all operations with earlier oplog times have been storage-committed.
	var confirmOp Oplog
	opts := mopts.FindOne().SetOplogReplay(true)
	coll := client.Database("local").Collection("oplog.rs")
	res := coll.FindOne(context.Background(), bson.M{"ts": bson.M{"$gte": latestOpTime}}, opts)
	if err := res.Err(); err != nil {
		return primitive.Timestamp{}, err
	}

	err = res.Decode(&confirmOp)
	if err == mongo.ErrNoDocuments {
		return primitive.Timestamp{}, fmt.Errorf("Last op was not confirmed. last op time: %v. confirmation time was not found.",
			latestOpTime)
	}
	if err != nil {
		return primitive.Timestamp{}, err
	}

	if !confirmOp.Timestamp.Equal(latestOpTime) {
		return primitive.Timestamp{}, fmt.Errorf("Last op was not confirmed. last op time: %v. confirmation time: %v",
			latestOpTime, confirmOp.Timestamp)
	}
	return latestOpTime, nil
}

// GetLatestOplogTimestamp returns the timestamp of the most recent oplog
// record satisfying the given `query` or a zero-value primitive.Timestamp if
// no oplog record matches.  This method does not ensure that all prior oplog
// entries are visible (i.e. have been storage-committed).
func GetLatestOplogTimestamp(client *mongo.Client, query interface{}) (primitive.Timestamp, error) {
	var record Oplog
	opts := mopts.FindOne().SetProjection(bson.M{"ts": 1}).SetSort(bson.D{{"$natural", -1}})
	coll := client.Database("local").Collection("oplog.rs")
	res := coll.FindOne(context.Background(), query, opts)
	if err := res.Err(); err != nil {
		return primitive.Timestamp{}, err
	}

	if err := res.Decode(&record); err != nil {
		return primitive.Timestamp{}, err
	}
	return record.Timestamp, nil
}
