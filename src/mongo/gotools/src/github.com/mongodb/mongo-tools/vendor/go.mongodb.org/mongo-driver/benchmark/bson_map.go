// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"context"
	"errors"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
)

func bsonMapDecoding(ctx context.Context, tm TimerManager, iters int, dataSet string) error {
	r, err := loadSourceRaw(getProjectRoot(), perfDataDir, bsonDataDir, dataSet)
	if err != nil {
		return err
	}

	tm.ResetTimer()

	for i := 0; i < iters; i++ {
		out := make(map[string]interface{})
		err := bson.Unmarshal(r, &out)
		if err != nil {
			return nil
		}
		if len(out) == 0 {
			return fmt.Errorf("decoding failed")
		}
	}
	return nil
}

func bsonMapEncoding(ctx context.Context, tm TimerManager, iters int, dataSet string) error {
	r, err := loadSourceRaw(getProjectRoot(), perfDataDir, bsonDataDir, dataSet)
	if err != nil {
		return err
	}

	doc := make(map[string]interface{})
	err = bson.Unmarshal(r, &doc)
	if err != nil {
		return err
	}

	var buf []byte
	tm.ResetTimer()
	for i := 0; i < iters; i++ {
		buf, err = bson.MarshalAppend(buf[:0], doc)
		if err != nil {
			return nil
		}

		if len(buf) == 0 {
			return errors.New("encoding failed")
		}
	}

	return nil
}

func BSONFlatMapDecoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonMapDecoding(ctx, tm, iters, flatBSONData)
}

func BSONFlatMapEncoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonMapEncoding(ctx, tm, iters, flatBSONData)
}

func BSONDeepMapDecoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonMapDecoding(ctx, tm, iters, deepBSONData)
}

func BSONDeepMapEncoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonMapEncoding(ctx, tm, iters, deepBSONData)
}

func BSONFullMapDecoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonMapDecoding(ctx, tm, iters, fullBSONData)
}

func BSONFullMapEncoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonMapEncoding(ctx, tm, iters, fullBSONData)
}
