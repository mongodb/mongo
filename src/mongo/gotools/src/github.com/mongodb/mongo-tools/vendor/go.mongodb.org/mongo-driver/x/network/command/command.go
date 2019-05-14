// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command // import "go.mongodb.org/mongo-driver/x/network/command"

import (
	"errors"

	"context"

	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsontype"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// WriteBatch represents a single batch for a write operation.
type WriteBatch struct {
	*Write
	numDocs int
}

// DecodeError attempts to decode the wiremessage as an error
func DecodeError(wm wiremessage.WireMessage) error {
	var rdr bson.Raw
	switch msg := wm.(type) {
	case wiremessage.Msg:
		for _, section := range msg.Sections {
			switch converted := section.(type) {
			case wiremessage.SectionBody:
				rdr = converted.Document
			}
		}
	case wiremessage.Reply:
		if msg.ResponseFlags&wiremessage.QueryFailure != wiremessage.QueryFailure {
			return nil
		}
		rdr = msg.Documents[0]
	}

	err := rdr.Validate()
	if err != nil {
		return nil
	}

	extractedError := extractError(rdr)

	// If parsed successfully return the error
	if _, ok := extractedError.(Error); ok {
		return err
	}

	return nil
}

// helper method to extract an error from a reader if there is one; first returned item is the
// error if it exists, the second holds parsing errors
func extractError(rdr bson.Raw) error {
	var errmsg, codeName string
	var code int32
	var labels []string
	elems, err := rdr.Elements()
	if err != nil {
		return err
	}

	for _, elem := range elems {
		switch elem.Key() {
		case "ok":
			switch elem.Value().Type {
			case bson.TypeInt32:
				if elem.Value().Int32() == 1 {
					return nil
				}
			case bson.TypeInt64:
				if elem.Value().Int64() == 1 {
					return nil
				}
			case bson.TypeDouble:
				if elem.Value().Double() == 1 {
					return nil
				}
			}
		case "errmsg":
			if str, okay := elem.Value().StringValueOK(); okay {
				errmsg = str
			}
		case "codeName":
			if str, okay := elem.Value().StringValueOK(); okay {
				codeName = str
			}
		case "code":
			if c, okay := elem.Value().Int32OK(); okay {
				code = c
			}
		case "errorLabels":
			if arr, okay := elem.Value().ArrayOK(); okay {
				elems, err := arr.Elements()
				if err != nil {
					continue
				}
				for _, elem := range elems {
					if str, ok := elem.Value().StringValueOK(); ok {
						labels = append(labels, str)
					}
				}

			}
		}
	}

	if errmsg == "" {
		errmsg = "command failed"
	}

	return Error{
		Code:    code,
		Message: errmsg,
		Name:    codeName,
		Labels:  labels,
	}
}

func responseClusterTime(response bson.Raw) bson.Raw {
	clusterTime, err := response.LookupErr("$clusterTime")
	if err != nil {
		// $clusterTime not included by the server
		return nil
	}
	idx, doc := bsoncore.AppendDocumentStart(nil)
	doc = bsoncore.AppendHeader(doc, clusterTime.Type, "$clusterTime")
	doc = append(doc, clusterTime.Value...)
	doc, _ = bsoncore.AppendDocumentEnd(doc, idx)
	return doc
}

func updateClusterTimes(sess *session.Client, clock *session.ClusterClock, response bson.Raw) error {
	clusterTime := responseClusterTime(response)
	if clusterTime == nil {
		return nil
	}

	if sess != nil {
		err := sess.AdvanceClusterTime(clusterTime)
		if err != nil {
			return err
		}
	}

	if clock != nil {
		clock.AdvanceClusterTime(clusterTime)
	}

	return nil
}

func updateOperationTime(sess *session.Client, response bson.Raw) error {
	if sess == nil {
		return nil
	}

	opTimeElem, err := response.LookupErr("operationTime")
	if err != nil {
		// operationTime not included by the server
		return nil
	}

	t, i := opTimeElem.Timestamp()
	return sess.AdvanceOperationTime(&primitive.Timestamp{
		T: t,
		I: i,
	})
}

func marshalCommand(cmd bsonx.Doc) (bson.Raw, error) {
	if cmd == nil {
		return bson.Raw{5, 0, 0, 0, 0}, nil
	}

	return cmd.MarshalBSON()
}

// adds session related fields to a BSON doc representing a command
func addSessionFields(cmd bsonx.Doc, desc description.SelectedServer, client *session.Client) (bsonx.Doc, error) {
	if client == nil || !description.SessionsSupported(desc.WireVersion) || desc.SessionTimeoutMinutes == 0 {
		return cmd, nil
	}

	if client.Terminated {
		return cmd, session.ErrSessionEnded
	}

	if _, err := cmd.LookupElementErr("lsid"); err != nil {
		cmd = cmd.Delete("lsid")
	}

	cmd = append(cmd, bsonx.Elem{"lsid", bsonx.Document(client.SessionID)})

	if client.TransactionRunning() ||
		client.RetryingCommit {
		cmd = addTransaction(cmd, client)
	}

	client.ApplyCommand(desc.Server) // advance the state machine based on a command executing

	return cmd, nil
}

// if in a transaction, add the transaction fields
func addTransaction(cmd bsonx.Doc, client *session.Client) bsonx.Doc {
	cmd = append(cmd, bsonx.Elem{"txnNumber", bsonx.Int64(client.TxnNumber)})
	if client.TransactionStarting() {
		// When starting transaction, always transition to the next state, even on error
		cmd = append(cmd, bsonx.Elem{"startTransaction", bsonx.Boolean(true)})
	}
	return append(cmd, bsonx.Elem{"autocommit", bsonx.Boolean(false)})
}

func addClusterTime(cmd bsonx.Doc, desc description.SelectedServer, sess *session.Client, clock *session.ClusterClock) bsonx.Doc {
	if (clock == nil && sess == nil) || !description.SessionsSupported(desc.WireVersion) {
		return cmd
	}

	var clusterTime bson.Raw
	if clock != nil {
		clusterTime = clock.GetClusterTime()
	}

	if sess != nil {
		if clusterTime == nil {
			clusterTime = sess.ClusterTime
		} else {
			clusterTime = session.MaxClusterTime(clusterTime, sess.ClusterTime)
		}
	}

	if clusterTime == nil {
		return cmd
	}

	d, err := bsonx.ReadDoc(clusterTime)
	if err != nil {
		return cmd // broken clusterTime
	}

	cmd = cmd.Delete("$clusterTime")

	return append(cmd, d...)
}

// add a read concern to a BSON doc representing a command
func addReadConcern(cmd bsonx.Doc, desc description.SelectedServer, rc *readconcern.ReadConcern, sess *session.Client) (bsonx.Doc, error) {
	// Starting transaction's read concern overrides all others
	if sess != nil && sess.TransactionStarting() && sess.CurrentRc != nil {
		rc = sess.CurrentRc
	}

	// start transaction must append afterclustertime IF causally consistent and operation time exists
	if rc == nil && sess != nil && sess.TransactionStarting() && sess.Consistent && sess.OperationTime != nil {
		rc = readconcern.New()
	}

	if rc == nil {
		return cmd, nil
	}

	t, data, err := rc.MarshalBSONValue()
	if err != nil {
		return cmd, err
	}

	var rcDoc bsonx.Doc
	err = rcDoc.UnmarshalBSONValue(t, data)
	if err != nil {
		return cmd, err
	}
	if description.SessionsSupported(desc.WireVersion) && sess != nil && sess.Consistent && sess.OperationTime != nil {
		rcDoc = append(rcDoc, bsonx.Elem{"afterClusterTime", bsonx.Timestamp(sess.OperationTime.T, sess.OperationTime.I)})
	}

	cmd = cmd.Delete("readConcern")

	if len(rcDoc) != 0 {
		cmd = append(cmd, bsonx.Elem{"readConcern", bsonx.Document(rcDoc)})
	}
	return cmd, nil
}

// add a write concern to a BSON doc representing a command
func addWriteConcern(cmd bsonx.Doc, wc *writeconcern.WriteConcern) (bsonx.Doc, error) {
	if wc == nil {
		return cmd, nil
	}

	t, data, err := wc.MarshalBSONValue()
	if err != nil {
		if err == writeconcern.ErrEmptyWriteConcern {
			return cmd, nil
		}
		return cmd, err
	}

	var xval bsonx.Val
	err = xval.UnmarshalBSONValue(t, data)
	if err != nil {
		return cmd, err
	}

	// delete if doc already has write concern
	cmd = cmd.Delete("writeConcern")

	return append(cmd, bsonx.Elem{Key: "writeConcern", Value: xval}), nil
}

// Get the error labels from a command response
func getErrorLabels(rdr *bson.Raw) ([]string, error) {
	var labels []string
	labelsElem, err := rdr.LookupErr("errorLabels")
	if err != bsoncore.ErrElementNotFound {
		return nil, err
	}
	if labelsElem.Type == bsontype.Array {
		labelsIt, err := labelsElem.Array().Elements()
		if err != nil {
			return nil, err
		}
		for _, elem := range labelsIt {
			labels = append(labels, elem.Value().StringValue())
		}
	}
	return labels, nil
}

// Remove command arguments for insert, update, and delete commands from the BSON document so they can be encoded
// as a Section 1 payload in OP_MSG
func opmsgRemoveArray(cmd bsonx.Doc) (bsonx.Doc, bsonx.Arr, string) {
	var array bsonx.Arr
	var id string

	keys := []string{"documents", "updates", "deletes"}

	for _, key := range keys {
		val, err := cmd.LookupErr(key)
		if err != nil {
			continue
		}

		array = val.Array()
		cmd = cmd.Delete(key)
		id = key
		break
	}

	return cmd, array, id
}

// Add the $db and $readPreference keys to the command
// If the command has no read preference, pass nil for rpDoc
func opmsgAddGlobals(cmd bsonx.Doc, dbName string, rpDoc bsonx.Doc) (bson.Raw, error) {
	cmd = append(cmd, bsonx.Elem{"$db", bsonx.String(dbName)})
	if rpDoc != nil {
		cmd = append(cmd, bsonx.Elem{"$readPreference", bsonx.Document(rpDoc)})
	}

	return cmd.MarshalBSON() // bsonx.Doc.MarshalBSON never returns an error.
}

func opmsgCreateDocSequence(arr bsonx.Arr, identifier string) (wiremessage.SectionDocumentSequence, error) {
	docSequence := wiremessage.SectionDocumentSequence{
		PayloadType: wiremessage.DocumentSequence,
		Identifier:  identifier,
		Documents:   make([]bson.Raw, 0, len(arr)),
	}

	for _, val := range arr {
		d, _ := val.Document().MarshalBSON()
		docSequence.Documents = append(docSequence.Documents, d)
	}

	docSequence.Size = int32(docSequence.PayloadLen())
	return docSequence, nil
}

func splitBatches(docs []bsonx.Doc, maxCount, targetBatchSize int) ([][]bsonx.Doc, error) {
	batches := [][]bsonx.Doc{}

	if targetBatchSize > reservedCommandBufferBytes {
		targetBatchSize -= reservedCommandBufferBytes
	}

	if maxCount <= 0 {
		maxCount = 1
	}

	startAt := 0
splitInserts:
	for {
		size := 0
		batch := []bsonx.Doc{}
	assembleBatch:
		for idx := startAt; idx < len(docs); idx++ {
			raw, _ := docs[idx].MarshalBSON()

			if len(raw) > targetBatchSize {
				return nil, ErrDocumentTooLarge
			}
			if size+len(raw) > targetBatchSize {
				break assembleBatch
			}

			size += len(raw)
			batch = append(batch, docs[idx])
			startAt++
			if len(batch) == maxCount {
				break assembleBatch
			}
		}
		batches = append(batches, batch)
		if startAt == len(docs) {
			break splitInserts
		}
	}

	return batches, nil
}

func encodeBatch(
	docs []bsonx.Doc,
	opts []bsonx.Elem,
	cmdKind WriteCommandKind,
	collName string,
) (bsonx.Doc, error) {
	var cmdName string
	var docString string

	switch cmdKind {
	case InsertCommand:
		cmdName = "insert"
		docString = "documents"
	case UpdateCommand:
		cmdName = "update"
		docString = "updates"
	case DeleteCommand:
		cmdName = "delete"
		docString = "deletes"
	}

	cmd := bsonx.Doc{{cmdName, bsonx.String(collName)}}

	vals := make(bsonx.Arr, 0, len(docs))
	for _, doc := range docs {
		vals = append(vals, bsonx.Document(doc))
	}
	cmd = append(cmd, bsonx.Elem{docString, bsonx.Array(vals)})
	cmd = append(cmd, opts...)

	return cmd, nil
}

// converts batches of Write Commands to wire messages
func batchesToWireMessage(batches []*WriteBatch, desc description.SelectedServer) ([]wiremessage.WireMessage, error) {
	wms := make([]wiremessage.WireMessage, len(batches))
	for _, cmd := range batches {
		wm, err := cmd.Encode(desc)
		if err != nil {
			return nil, err
		}

		wms = append(wms, wm)
	}

	return wms, nil
}

// Roundtrips the write batches, returning the result structs (as interface),
// the write batches that weren't round tripped and any errors
func roundTripBatches(
	ctx context.Context,
	desc description.SelectedServer,
	rw wiremessage.ReadWriter,
	batches []*WriteBatch,
	continueOnError bool,
	sess *session.Client,
	cmdKind WriteCommandKind,
) (interface{}, []*WriteBatch, error) {
	var res interface{}
	var upsertIndex int64 // the operation index for the upserted IDs map

	// hold onto txnNumber, reset it when loop exits to ensure reuse of same
	// transaction number if retry is needed
	var txnNumber int64
	if sess != nil && sess.RetryWrite {
		txnNumber = sess.TxnNumber
	}
	for j, cmd := range batches {
		rdr, err := cmd.RoundTrip(ctx, desc, rw)
		if err != nil {
			if sess != nil && sess.RetryWrite {
				sess.TxnNumber = txnNumber + int64(j)
			}
			return res, batches, err
		}

		// TODO can probably DRY up this code
		switch cmdKind {
		case InsertCommand:
			if res == nil {
				res = result.Insert{}
			}

			conv, _ := res.(result.Insert)
			insertCmd := &Insert{}
			r, err := insertCmd.decode(desc, rdr).Result()
			if err != nil {
				return res, batches, err
			}

			conv.WriteErrors = append(conv.WriteErrors, r.WriteErrors...)

			if r.WriteConcernError != nil {
				conv.WriteConcernError = r.WriteConcernError
				if sess != nil && sess.RetryWrite {
					sess.TxnNumber = txnNumber
					return conv, batches, nil // report writeconcernerror for retry
				}
			}

			conv.N += r.N

			if !continueOnError && len(conv.WriteErrors) > 0 {
				return conv, batches, nil
			}

			res = conv
		case UpdateCommand:
			if res == nil {
				res = result.Update{}
			}

			conv, _ := res.(result.Update)
			updateCmd := &Update{}
			r, err := updateCmd.decode(desc, rdr).Result()
			if err != nil {
				return conv, batches, err
			}

			conv.WriteErrors = append(conv.WriteErrors, r.WriteErrors...)

			if r.WriteConcernError != nil {
				conv.WriteConcernError = r.WriteConcernError
				if sess != nil && sess.RetryWrite {
					sess.TxnNumber = txnNumber
					return conv, batches, nil // report writeconcernerror for retry
				}
			}

			conv.MatchedCount += r.MatchedCount
			conv.ModifiedCount += r.ModifiedCount
			for _, upsert := range r.Upserted {
				conv.Upserted = append(conv.Upserted, result.Upsert{
					Index: upsert.Index + upsertIndex,
					ID:    upsert.ID,
				})
			}

			if !continueOnError && len(conv.WriteErrors) > 0 {
				return conv, batches, nil
			}

			res = conv
			upsertIndex += int64(cmd.numDocs)
		case DeleteCommand:
			if res == nil {
				res = result.Delete{}
			}

			conv, _ := res.(result.Delete)
			deleteCmd := &Delete{}
			r, err := deleteCmd.decode(desc, rdr).Result()
			if err != nil {
				return conv, batches, err
			}

			conv.WriteErrors = append(conv.WriteErrors, r.WriteErrors...)

			if r.WriteConcernError != nil {
				conv.WriteConcernError = r.WriteConcernError
				if sess != nil && sess.RetryWrite {
					sess.TxnNumber = txnNumber
					return conv, batches, nil // report writeconcernerror for retry
				}
			}

			conv.N += r.N

			if !continueOnError && len(conv.WriteErrors) > 0 {
				return conv, batches, nil
			}

			res = conv
		}

		// Increment txnNumber for each batch
		if sess != nil && sess.RetryWrite {
			sess.IncrementTxnNumber()
			batches = batches[1:] // if batch encoded successfully, remove it from the slice
		}
	}

	if sess != nil && sess.RetryWrite {
		// if retryable write succeeded, transaction number will be incremented one extra time,
		// so we decrement it here
		sess.TxnNumber--
	}

	return res, batches, nil
}

// get the firstBatch, cursor ID, and namespace from a bson.Raw
func getCursorValues(result bson.Raw) ([]bson.RawValue, Namespace, int64, error) {
	cur, err := result.LookupErr("cursor")
	if err != nil {
		return nil, Namespace{}, 0, err
	}
	if cur.Type != bson.TypeEmbeddedDocument {
		return nil, Namespace{}, 0, fmt.Errorf("cursor should be an embedded document but it is a BSON %s", cur.Type)
	}

	elems, err := cur.Document().Elements()
	if err != nil {
		return nil, Namespace{}, 0, err
	}

	var ok bool
	var arr bson.Raw
	var namespace Namespace
	var cursorID int64

	for _, elem := range elems {
		switch elem.Key() {
		case "firstBatch":
			arr, ok = elem.Value().ArrayOK()
			if !ok {
				return nil, Namespace{}, 0, fmt.Errorf("firstBatch should be an array but it is a BSON %s", elem.Value().Type)
			}
			if err != nil {
				return nil, Namespace{}, 0, err
			}
		case "ns":
			if elem.Value().Type != bson.TypeString {
				return nil, Namespace{}, 0, fmt.Errorf("namespace should be a string but it is a BSON %s", elem.Value().Type)
			}
			namespace = ParseNamespace(elem.Value().StringValue())
			err = namespace.Validate()
			if err != nil {
				return nil, Namespace{}, 0, err
			}
		case "id":
			cursorID, ok = elem.Value().Int64OK()
			if !ok {
				return nil, Namespace{}, 0, fmt.Errorf("id should be an int64 but it is a BSON %s", elem.Value().Type)
			}
		}
	}

	vals, err := arr.Values()
	if err != nil {
		return nil, Namespace{}, 0, err
	}

	return vals, namespace, cursorID, nil
}

func getBatchSize(opts []bsonx.Elem) int32 {
	for _, opt := range opts {
		if opt.Key == "batchSize" {
			return opt.Value.Int32()
		}
	}

	return 0
}

// ErrUnacknowledgedWrite is returned from functions that have an unacknowledged
// write concern.
var ErrUnacknowledgedWrite = errors.New("unacknowledged write")

// WriteCommandKind is the type of command represented by a Write
type WriteCommandKind int8

// These constants represent the valid types of write commands.
const (
	InsertCommand WriteCommandKind = iota
	UpdateCommand
	DeleteCommand
)
