// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/bson"
)

func BSONFlatStructDecoding(ctx context.Context, tm TimerManager, iters int) error {
	r, err := loadSourceRaw(getProjectRoot(), perfDataDir, bsonDataDir, flatBSONData)
	if err != nil {
		return err
	}

	tm.ResetTimer()

	for i := 0; i < iters; i++ {
		out := flatBSON{}
		err := bson.Unmarshal(r, &out)
		if err != nil {
			return err
		}
	}
	return nil
}

func BSONFlatStructEncoding(ctx context.Context, tm TimerManager, iters int) error {
	r, err := loadSourceRaw(getProjectRoot(), perfDataDir, bsonDataDir, flatBSONData)
	if err != nil {
		return err
	}

	doc := flatBSON{}
	err = bson.Unmarshal(r, &doc)
	if err != nil {
		return err
	}

	var buf []byte

	tm.ResetTimer()
	for i := 0; i < iters; i++ {
		buf, err = bson.Marshal(doc)
		if err != nil {
			return err
		}
		if len(buf) == 0 {
			return errors.New("encoding failed")
		}
	}
	return nil
}

func BSONFlatStructTagsEncoding(ctx context.Context, tm TimerManager, iters int) error {
	r, err := loadSourceRaw(getProjectRoot(), perfDataDir, bsonDataDir, flatBSONData)
	if err != nil {
		return err
	}

	doc := flatBSONTags{}
	err = bson.Unmarshal(r, &doc)
	if err != nil {
		return err
	}

	var buf []byte

	tm.ResetTimer()
	for i := 0; i < iters; i++ {
		buf, err = bson.MarshalAppend(buf[:0], doc)
		if err != nil {
			return err
		}
		if len(buf) == 0 {
			return errors.New("encoding failed")
		}
	}
	return nil
}

func BSONFlatStructTagsDecoding(ctx context.Context, tm TimerManager, iters int) error {
	r, err := loadSourceRaw(getProjectRoot(), perfDataDir, bsonDataDir, flatBSONData)
	if err != nil {
		return err
	}

	tm.ResetTimer()
	for i := 0; i < iters; i++ {
		out := flatBSONTags{}
		err := bson.Unmarshal(r, &out)
		if err != nil {
			return err
		}
	}
	return nil
}
