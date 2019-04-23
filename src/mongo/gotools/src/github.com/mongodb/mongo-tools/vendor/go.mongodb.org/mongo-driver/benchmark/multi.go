// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/x/bsonx"
)

func MultiFindMany(ctx context.Context, tm TimerManager, iters int) error {
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

	doc, err := loadSourceDocument(getProjectRoot(), perfDataDir, singleAndMultiDataDir, tweetData)
	if err != nil {
		return err
	}

	coll := db.Collection("corpus")

	payload := make([]interface{}, iters)
	for idx := range payload {
		payload[idx] = doc
	}

	if _, err = coll.InsertMany(ctx, payload); err != nil {
		return err
	}

	tm.ResetTimer()

	cursor, err := coll.Find(ctx, bsonx.Doc{})
	if err != nil {
		return err
	}
	defer cursor.Close(ctx)

	counter := 0
	for cursor.Next(ctx) {
		err = cursor.Err()
		if err != nil {
			return err
		}
		if len(cursor.Current) == 0 {
			return errors.New("error retrieving document")
		}

		counter++
	}

	if counter != iters {
		return errors.New("problem iterating cursors")

	}

	tm.StopTimer()

	if err = cursor.Close(ctx); err != nil {
		return err
	}

	if err = db.Drop(ctx); err != nil {
		return err
	}

	return nil
}

func multiInsertCase(ctx context.Context, tm TimerManager, iters int, data string) error {
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

	payload := make([]interface{}, iters)
	for idx := range payload {
		payload[idx] = doc
	}

	coll := db.Collection("corpus")

	tm.ResetTimer()
	res, err := coll.InsertMany(ctx, payload)
	if err != nil {
		return err
	}
	tm.StopTimer()

	if len(res.InsertedIDs) != iters {
		return errors.New("bulk operation did not complete")
	}

	if err = db.Drop(ctx); err != nil {
		return err
	}

	return nil
}

func MultiInsertSmallDocument(ctx context.Context, tm TimerManager, iters int) error {
	return multiInsertCase(ctx, tm, iters, smallData)
}

func MultiInsertLargeDocument(ctx context.Context, tm TimerManager, iters int) error {
	return multiInsertCase(ctx, tm, iters, largeData)
}
