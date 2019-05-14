// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"context"

	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
)

// BulkWriteError is an error from one operation in a bulk write.
type BulkWriteError struct {
	result.WriteError
	Model WriteModel
}

// BulkWriteException is a collection of errors returned by a bulk write operation.
type BulkWriteException struct {
	WriteConcernError *result.WriteConcernError
	WriteErrors       []BulkWriteError
}

func (BulkWriteException) Error() string {
	return ""
}

type bulkWriteBatch struct {
	models   []WriteModel
	canRetry bool
}

// BulkWrite handles the full dispatch cycle for a bulk write operation.
func BulkWrite(
	ctx context.Context,
	ns command.Namespace,
	models []WriteModel,
	topo *topology.Topology,
	selector description.ServerSelector,
	clientID uuid.UUID,
	pool *session.Pool,
	retryWrite bool,
	sess *session.Client,
	writeConcern *writeconcern.WriteConcern,
	clock *session.ClusterClock,
	registry *bsoncodec.Registry,
	opts ...*options.BulkWriteOptions,
) (result.BulkWrite, error) {
	if sess != nil && sess.PinnedServer != nil {
		selector = sess.PinnedServer
	}
	ss, err := topo.SelectServer(ctx, selector)
	if err != nil {
		return result.BulkWrite{}, err
	}

	err = verifyOptions(models, ss)
	if err != nil {
		return result.BulkWrite{}, err
	}

	// If no explicit session and deployment supports sessions, start implicit session.
	if sess == nil && topo.SupportsSessions() {
		sess, err = session.NewClientSession(pool, clientID, session.Implicit)
		if err != nil {
			return result.BulkWrite{}, err
		}

		defer sess.EndSession()
	}

	bwOpts := options.MergeBulkWriteOptions(opts...)

	ordered := *bwOpts.Ordered

	batches := createBatches(models, ordered)
	bwRes := result.BulkWrite{
		UpsertedIDs: make(map[int64]interface{}),
	}
	bwErr := BulkWriteException{
		WriteErrors: make([]BulkWriteError, 0),
	}

	var lastErr error
	var opIndex int64 // the operation index for the upsertedIDs map
	continueOnError := !ordered
	for _, batch := range batches {
		if len(batch.models) == 0 {
			continue
		}

		batchRes, batchErr, err := runBatch(ctx, ns, topo, selector, ss, sess, clock, writeConcern, retryWrite,
			bwOpts.BypassDocumentValidation, continueOnError, batch, registry)

		mergeResults(&bwRes, batchRes, opIndex)
		bwErr.WriteConcernError = batchErr.WriteConcernError
		for i := range batchErr.WriteErrors {
			batchErr.WriteErrors[i].Index = batchErr.WriteErrors[i].Index + int(opIndex)
		}
		bwErr.WriteErrors = append(bwErr.WriteErrors, batchErr.WriteErrors...)

		if !continueOnError && (err != nil || len(batchErr.WriteErrors) > 0 || batchErr.WriteConcernError != nil) {
			if err != nil {
				return bwRes, err
			}

			return bwRes, bwErr
		}

		if err != nil {
			lastErr = err
		}

		opIndex += int64(len(batch.models))
	}

	bwRes.MatchedCount -= bwRes.UpsertedCount
	if lastErr != nil {
		return bwRes, lastErr
	}
	if len(bwErr.WriteErrors) > 0 || bwErr.WriteConcernError != nil {
		return bwRes, bwErr
	}
	return bwRes, nil
}

func runBatch(
	ctx context.Context,
	ns command.Namespace,
	topo *topology.Topology,
	selector description.ServerSelector,
	ss *topology.SelectedServer,
	sess *session.Client,
	clock *session.ClusterClock,
	wc *writeconcern.WriteConcern,
	retryWrite bool,
	bypassDocValidation *bool,
	continueOnError bool,
	batch bulkWriteBatch,
	registry *bsoncodec.Registry,
) (result.BulkWrite, BulkWriteException, error) {
	batchRes := result.BulkWrite{
		UpsertedIDs: make(map[int64]interface{}),
	}
	batchErr := BulkWriteException{}

	var writeErrors []result.WriteError
	switch batch.models[0].(type) {
	case InsertOneModel:
		res, err := runInsert(ctx, ns, topo, selector, ss, sess, clock, wc, retryWrite, batch, bypassDocValidation,
			continueOnError, registry)
		if err != nil {
			return result.BulkWrite{}, BulkWriteException{}, err
		}

		batchRes.InsertedCount = int64(res.N)
		writeErrors = res.WriteErrors
		batchErr.WriteConcernError = res.WriteConcernError
	case DeleteOneModel, DeleteManyModel:
		res, err := runDelete(ctx, ns, topo, selector, ss, sess, clock, wc, retryWrite, batch, continueOnError, registry)
		if err != nil {
			return result.BulkWrite{}, BulkWriteException{}, err
		}

		batchRes.DeletedCount = int64(res.N)
		writeErrors = res.WriteErrors
		batchErr.WriteConcernError = res.WriteConcernError
	case ReplaceOneModel, UpdateOneModel, UpdateManyModel:
		res, err := runUpdate(ctx, ns, topo, selector, ss, sess, clock, wc, retryWrite, batch, bypassDocValidation,
			continueOnError, registry)
		if err != nil {
			return result.BulkWrite{}, BulkWriteException{}, err
		}

		batchRes.MatchedCount = res.MatchedCount
		batchRes.ModifiedCount = res.ModifiedCount
		batchRes.UpsertedCount = int64(len(res.Upserted))
		writeErrors = res.WriteErrors
		batchErr.WriteConcernError = res.WriteConcernError
		for _, upsert := range res.Upserted {
			batchRes.UpsertedIDs[upsert.Index] = upsert.ID
		}
	}

	batchErr.WriteErrors = make([]BulkWriteError, 0, len(writeErrors))
	for _, we := range writeErrors {
		batchErr.WriteErrors = append(batchErr.WriteErrors, BulkWriteError{
			WriteError: we,
			Model:      batch.models[0],
		})
	}

	return batchRes, batchErr, nil
}

func runInsert(
	ctx context.Context,
	ns command.Namespace,
	topo *topology.Topology,
	selector description.ServerSelector,
	ss *topology.SelectedServer,
	sess *session.Client,
	clock *session.ClusterClock,
	wc *writeconcern.WriteConcern,
	retryWrite bool,
	batch bulkWriteBatch,
	bypassDocValidation *bool,
	continueOnError bool,
	registry *bsoncodec.Registry,
) (result.Insert, error) {
	docs := make([]bsonx.Doc, len(batch.models))
	var i int
	for _, model := range batch.models {
		converted := model.(InsertOneModel)
		doc, err := interfaceToDocument(converted.Document, registry)
		if err != nil {
			return result.Insert{}, err
		}

		docs[i] = doc
		i++
	}

	cmd := command.Insert{
		ContinueOnError: continueOnError,
		NS:              ns,
		Docs:            docs,
		Session:         sess,
		Clock:           clock,
		WriteConcern:    wc,
	}

	cmd.Opts = []bsonx.Elem{{"ordered", bsonx.Boolean(!continueOnError)}}
	if bypassDocValidation != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"bypassDocumentValidation", bsonx.Boolean(*bypassDocValidation)})
	}

	if !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) || !retryWrite || !batch.canRetry {
		if cmd.Session != nil {
			cmd.Session.RetryWrite = false
		}
		return insert(ctx, &cmd, ss, nil)
	}

	cmd.Session.RetryWrite = retryWrite
	cmd.Session.IncrementTxnNumber()

	res, origErr := insert(ctx, &cmd, ss, nil)
	if shouldRetry(origErr, res.WriteConcernError) {
		newServer, err := topo.SelectServer(ctx, selector)
		if err != nil || !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) {
			return res, origErr
		}

		return insert(ctx, &cmd, newServer, origErr)
	}

	return res, origErr
}

func runDelete(
	ctx context.Context,
	ns command.Namespace,
	topo *topology.Topology,
	selector description.ServerSelector,
	ss *topology.SelectedServer,
	sess *session.Client,
	clock *session.ClusterClock,
	wc *writeconcern.WriteConcern,
	retryWrite bool,
	batch bulkWriteBatch,
	continueOnError bool,
	registry *bsoncodec.Registry,
) (result.Delete, error) {
	docs := make([]bsonx.Doc, len(batch.models))
	var i int

	for _, model := range batch.models {
		var doc bsonx.Doc
		var err error

		if dom, ok := model.(DeleteOneModel); ok {
			doc, err = createDeleteDoc(dom.Filter, dom.Collation, false, registry)
		} else if dmm, ok := model.(DeleteManyModel); ok {
			doc, err = createDeleteDoc(dmm.Filter, dmm.Collation, true, registry)
		}

		if err != nil {
			return result.Delete{}, err
		}

		docs[i] = doc
		i++
	}

	cmd := command.Delete{
		ContinueOnError: continueOnError,
		NS:              ns,
		Deletes:         docs,
		Session:         sess,
		Clock:           clock,
		WriteConcern:    wc,
	}
	cmd.Opts = []bsonx.Elem{{"ordered", bsonx.Boolean(!continueOnError)}}

	if !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) || !retryWrite || !batch.canRetry {
		if cmd.Session != nil {
			cmd.Session.RetryWrite = false
		}
		return delete(ctx, &cmd, ss, nil)
	}

	cmd.Session.RetryWrite = retryWrite
	cmd.Session.IncrementTxnNumber()

	res, origErr := delete(ctx, &cmd, ss, nil)
	if shouldRetry(origErr, res.WriteConcernError) {
		newServer, err := topo.SelectServer(ctx, selector)
		if err != nil || !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) {
			return res, origErr
		}

		return delete(ctx, &cmd, newServer, origErr)
	}

	return res, origErr
}

func runUpdate(
	ctx context.Context,
	ns command.Namespace,
	topo *topology.Topology,
	selector description.ServerSelector,
	ss *topology.SelectedServer,
	sess *session.Client,
	clock *session.ClusterClock,
	wc *writeconcern.WriteConcern,
	retryWrite bool,
	batch bulkWriteBatch,
	bypassDocValidation *bool,
	continueOnError bool,
	registry *bsoncodec.Registry,
) (result.Update, error) {
	docs := make([]bsonx.Doc, len(batch.models))

	for i, model := range batch.models {
		var doc bsonx.Doc
		var err error

		if rom, ok := model.(ReplaceOneModel); ok {
			doc, err = createUpdateDoc(rom.Filter, rom.Replacement, options.ArrayFilters{}, false, rom.UpdateModel, false,
				registry)
		} else if uom, ok := model.(UpdateOneModel); ok {
			doc, err = createUpdateDoc(uom.Filter, uom.Update, uom.ArrayFilters, uom.ArrayFiltersSet, uom.UpdateModel, false,
				registry)
		} else if umm, ok := model.(UpdateManyModel); ok {
			doc, err = createUpdateDoc(umm.Filter, umm.Update, umm.ArrayFilters, umm.ArrayFiltersSet, umm.UpdateModel, true,
				registry)
		}

		if err != nil {
			return result.Update{}, err
		}

		docs[i] = doc
	}

	cmd := command.Update{
		ContinueOnError: continueOnError,
		NS:              ns,
		Docs:            docs,
		Session:         sess,
		Clock:           clock,
		WriteConcern:    wc,
	}

	cmd.Opts = []bsonx.Elem{{"ordered", bsonx.Boolean(!continueOnError)}}
	if bypassDocValidation != nil {
		// TODO this is temporary!
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"bypassDocumentValidation", bsonx.Boolean(*bypassDocValidation)})
		//cmd.Opts = []option.UpdateOptioner{option.OptBypassDocumentValidation(bypassDocValidation)}
	}

	if !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) || !retryWrite || !batch.canRetry {
		if cmd.Session != nil {
			cmd.Session.RetryWrite = false
		}
		return update(ctx, &cmd, ss, nil)
	}

	cmd.Session.RetryWrite = retryWrite
	cmd.Session.IncrementTxnNumber()

	res, origErr := update(ctx, &cmd, ss, nil)
	if shouldRetry(origErr, res.WriteConcernError) {
		newServer, err := topo.SelectServer(ctx, selector)
		if err != nil || !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) {
			return res, origErr
		}

		return update(ctx, &cmd, newServer, origErr)
	}

	return res, origErr
}

func verifyOptions(models []WriteModel, ss *topology.SelectedServer) error {
	maxVersion := ss.Description().WireVersion.Max
	// 3.4 is wire version 5
	// 3.6 is wire version 6

	for _, model := range models {
		var collationSet bool
		var afSet bool // arrayFilters

		switch converted := model.(type) {
		case DeleteOneModel:
			collationSet = converted.Collation != nil
		case DeleteManyModel:
			collationSet = converted.Collation != nil
		case ReplaceOneModel:
			collationSet = converted.Collation != nil
		case UpdateOneModel:
			afSet = converted.ArrayFiltersSet
			collationSet = converted.Collation != nil
		case UpdateManyModel:
			afSet = converted.ArrayFiltersSet
			collationSet = converted.Collation != nil
		}

		if afSet && maxVersion < 6 {
			return ErrArrayFilters
		}

		if collationSet && maxVersion < 5 {
			return ErrCollation
		}
	}

	return nil
}

func createBatches(models []WriteModel, ordered bool) []bulkWriteBatch {
	if ordered {
		return createOrderedBatches(models)
	}

	batches := make([]bulkWriteBatch, 3)
	var i int
	for i = 0; i < 3; i++ {
		batches[i].canRetry = true
	}

	var numBatches int // number of batches used. can't use len(batches) because it's set to 3
	insertInd := -1
	updateInd := -1
	deleteInd := -1

	for _, model := range models {
		switch converted := model.(type) {
		case InsertOneModel:
			if insertInd == -1 {
				// this is the first InsertOneModel
				insertInd = numBatches
				numBatches++
			}

			batches[insertInd].models = append(batches[insertInd].models, model)
		case DeleteOneModel, DeleteManyModel:
			if deleteInd == -1 {
				deleteInd = numBatches
				numBatches++
			}

			batches[deleteInd].models = append(batches[deleteInd].models, model)
			if _, ok := converted.(DeleteManyModel); ok {
				batches[deleteInd].canRetry = false
			}
		case ReplaceOneModel, UpdateOneModel, UpdateManyModel:
			if updateInd == -1 {
				updateInd = numBatches
				numBatches++
			}

			batches[updateInd].models = append(batches[updateInd].models, model)
			if _, ok := converted.(UpdateManyModel); ok {
				batches[updateInd].canRetry = false
			}
		}
	}

	return batches
}

func createOrderedBatches(models []WriteModel) []bulkWriteBatch {
	var batches []bulkWriteBatch
	var prevKind command.WriteCommandKind = -1
	i := -1 // batch index

	for _, model := range models {
		var createNewBatch bool
		var canRetry bool
		var newKind command.WriteCommandKind

		switch model.(type) {
		case InsertOneModel:
			createNewBatch = prevKind != command.InsertCommand
			canRetry = true
			newKind = command.InsertCommand
		case DeleteOneModel:
			createNewBatch = prevKind != command.DeleteCommand
			canRetry = true
			newKind = command.DeleteCommand
		case DeleteManyModel:
			createNewBatch = prevKind != command.DeleteCommand
			newKind = command.DeleteCommand
		case ReplaceOneModel, UpdateOneModel:
			createNewBatch = prevKind != command.UpdateCommand
			canRetry = true
			newKind = command.UpdateCommand
		case UpdateManyModel:
			createNewBatch = prevKind != command.UpdateCommand
			newKind = command.UpdateCommand
		}

		if createNewBatch {
			batches = append(batches, bulkWriteBatch{
				models:   []WriteModel{model},
				canRetry: canRetry,
			})
			i++
		} else {
			batches[i].models = append(batches[i].models, model)
			if !canRetry {
				batches[i].canRetry = false // don't make it true if it was already false
			}
		}

		prevKind = newKind
	}

	return batches
}

func shouldRetry(cmdErr error, wcErr *result.WriteConcernError) bool {
	if cerr, ok := cmdErr.(command.Error); ok && cerr.Retryable() ||
		wcErr != nil && command.IsWriteConcernErrorRetryable(wcErr) {
		return true
	}

	return false
}

func createUpdateDoc(
	filter interface{},
	update interface{},
	arrayFilters options.ArrayFilters,
	arrayFiltersSet bool,
	updateModel UpdateModel,
	multi bool,
	registry *bsoncodec.Registry,
) (bsonx.Doc, error) {
	f, err := interfaceToDocument(filter, registry)
	if err != nil {
		return nil, err
	}

	u, err := interfaceToDocument(update, registry)
	if err != nil {
		return nil, err
	}

	doc := bsonx.Doc{
		{"q", bsonx.Document(f)},
		{"u", bsonx.Document(u)},
		{"multi", bsonx.Boolean(multi)},
	}

	if arrayFiltersSet {
		filters, err := arrayFilters.ToArray()
		if err != nil {
			return nil, err
		}
		arr := make(bsonx.Arr, 0, len(filters))
		for _, filter := range filters {
			doc, err := bsonx.ReadDoc(filter)
			if err != nil {
				return nil, err
			}
			arr = append(arr, bsonx.Document(doc))
		}
		doc = append(doc, bsonx.Elem{"arrayFilters", bsonx.Array(arr)})
	}

	if updateModel.Collation != nil {
		collDoc, err := bsonx.ReadDoc(updateModel.Collation.ToDocument())
		if err != nil {
			return nil, err
		}
		doc = append(doc, bsonx.Elem{"collation", bsonx.Document(collDoc)})
	}

	if updateModel.UpsertSet {
		doc = append(doc, bsonx.Elem{"upsert", bsonx.Boolean(updateModel.Upsert)})
	}

	return doc, nil
}

func createDeleteDoc(
	filter interface{},
	collation *options.Collation,
	many bool,
	registry *bsoncodec.Registry,
) (bsonx.Doc, error) {
	f, err := interfaceToDocument(filter, registry)
	if err != nil {
		return nil, err
	}

	var limit int32 = 1
	if many {
		limit = 0
	}

	doc := bsonx.Doc{
		{"q", bsonx.Document(f)},
		{"limit", bsonx.Int32(limit)},
	}

	if collation != nil {
		collDoc, err := bsonx.ReadDoc(collation.ToDocument())
		if err != nil {
			return nil, err
		}
		doc = append(doc, bsonx.Elem{"collation", bsonx.Document(collDoc)})
	}

	return doc, nil
}

func mergeResults(aggResult *result.BulkWrite, newResult result.BulkWrite, opIndex int64) {
	aggResult.InsertedCount += newResult.InsertedCount
	aggResult.MatchedCount += newResult.MatchedCount
	aggResult.ModifiedCount += newResult.ModifiedCount
	aggResult.DeletedCount += newResult.DeletedCount
	aggResult.UpsertedCount += newResult.UpsertedCount

	for index, upsertID := range newResult.UpsertedIDs {
		aggResult.UpsertedIDs[index+opIndex] = upsertID
	}
}
