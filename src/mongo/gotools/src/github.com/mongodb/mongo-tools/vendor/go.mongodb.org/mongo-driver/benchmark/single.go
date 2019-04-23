// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/internal/testutil"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/bsonx"
)

const (
	singleAndMultiDataDir = "single_and_multi_document"
	tweetData             = "tweet.json"
	smallData             = "small_doc.json"
	largeData             = "large_doc.json"
)

func getClientDB(ctx context.Context) (*mongo.Database, error) {
	cs, err := testutil.GetConnString()
	if err != nil {
		return nil, err
	}
	client, err := mongo.NewClient(options.Client().ApplyURI(cs.String()))
	if err != nil {
		return nil, err
	}
	if err = client.Connect(ctx); err != nil {
		return nil, err
	}

	db := client.Database(testutil.GetDBName(cs))
	return db, nil
}

func SingleRunCommand(ctx context.Context, tm TimerManager, iters int) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	db, err := getClientDB(ctx)
	if err != nil {
		return err
	}
	defer db.Client().Disconnect(ctx)

	cmd := bsonx.Doc{{"ismaster", bsonx.Boolean(true)}}

	tm.ResetTimer()
	for i := 0; i < iters; i++ {
		var doc bsonx.Doc
		err := db.RunCommand(ctx, cmd).Decode(&doc)
		if err != nil {
			return err
		}
		// read the document and then throw it away to prevent
		out, err := doc.MarshalBSON()
		if len(out) == 0 {
			return errors.New("output of ismaster is empty")
		}
	}
	tm.StopTimer()

	return nil
}

func SingleFindOneByID(ctx context.Context, tm TimerManager, iters int) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	db, err := getClientDB(ctx)
	if err != nil {
		return err
	}

	db = db.Client().Database("perftest")
	if err = db.Drop(ctx); err != nil {
		return err
	}

	doc, err := loadSourceDocument(getProjectRoot(), perfDataDir, singleAndMultiDataDir, tweetData)
	if err != nil {
		return err
	}
	coll := db.Collection("corpus")
	for i := 0; i < iters; i++ {
		id := int32(i)
		res, err := coll.InsertOne(ctx, doc.Set("_id", bsonx.Int32(id)))
		if err != nil {
			return err
		}
		if res.InsertedID == nil {
			return errors.New("insert failed")
		}
	}

	tm.ResetTimer()

	for i := 0; i < iters; i++ {
		res := coll.FindOne(ctx, bsonx.Doc{{"_id", bsonx.Int32(int32(i))}})
		if res == nil {
			return errors.New("find one query produced nil result")
		}
	}

	tm.StopTimer()

	if err = db.Drop(ctx); err != nil {
		return err
	}

	return nil
}

func singleInsertCase(ctx context.Context, tm TimerManager, iters int, data string) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	db, err := getClientDB(ctx)
	if err != nil {
		return err
	}
	defer db.Client().Disconnect(ctx)

	db = db.Client().Database("perftest")
	if err = db.Drop(ctx); err != nil {
		return err
	}

	doc, err := loadSourceDocument(getProjectRoot(), perfDataDir, singleAndMultiDataDir, data)
	if err != nil {
		return err
	}

	err = db.RunCommand(ctx, bsonx.Doc{{"create", bsonx.String("corpus")}}).Err()
	if err != nil {
		return err
	}

	coll := db.Collection("corpus")

	tm.ResetTimer()

	for i := 0; i < iters; i++ {
		if _, err = coll.InsertOne(ctx, doc); err != nil {
			return err
		}

		// TODO: should be remove after resolving GODRIVER-468
		_ = doc.Delete("_id")
	}

	tm.StopTimer()

	if err = db.Drop(ctx); err != nil {
		return err
	}

	return nil
}

func SingleInsertSmallDocument(ctx context.Context, tm TimerManager, iters int) error {
	return singleInsertCase(ctx, tm, iters, smallData)
}

func SingleInsertLargeDocument(ctx context.Context, tm TimerManager, iters int) error {
	return singleInsertCase(ctx, tm, iters, largeData)
}
