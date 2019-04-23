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

func bsonDocumentEncoding(ctx context.Context, tm TimerManager, iters int, source string) error {
	doc, err := loadSourceDocument(getProjectRoot(), perfDataDir, bsonDataDir, source)
	if err != nil {
		return err
	}

	tm.ResetTimer()

	for i := 0; i < iters; i++ {
		out, err := doc.MarshalBSON()
		if err != nil {
			return err
		}
		if len(out) == 0 {
			return errors.New("marshaling error")
		}
	}

	return nil
}

func bsonDocumentDecodingLazy(ctx context.Context, tm TimerManager, iters int, source string) error {
	doc, err := loadSourceDocument(getProjectRoot(), perfDataDir, bsonDataDir, source)
	if err != nil {
		return err
	}

	raw, err := doc.MarshalBSON()
	if err != nil {
		return err
	}

	tm.ResetTimer()

	for i := 0; i < iters; i++ {
		out, err := bsonx.ReadDoc(raw)
		if err != nil {
			return err
		}
		if len(out) == 0 {
			return errors.New("marshaling error")
		}
	}
	return nil
}

func bsonDocumentDecoding(ctx context.Context, tm TimerManager, iters, numKeys int, source string) error {
	doc, err := loadSourceDocument(getProjectRoot(), perfDataDir, bsonDataDir, source)
	if err != nil {
		return err
	}

	raw, err := doc.MarshalBSON()
	if err != nil {
		return err
	}

	tm.ResetTimer()

	for i := 0; i < iters; i++ {
		out, err := bsonx.ReadDoc(raw)
		if err != nil {
			return err
		}

		if len(out) != numKeys {
			return errors.New("document parsing error")
		}
	}
	return nil

}

func BSONFlatDocumentEncoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentEncoding(ctx, tm, iters, flatBSONData)
}

func BSONFlatDocumentDecodingLazy(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentDecodingLazy(ctx, tm, iters, flatBSONData)
}

func BSONFlatDocumentDecoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentDecoding(ctx, tm, iters, 145, flatBSONData)
}

func BSONDeepDocumentEncoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentEncoding(ctx, tm, iters, deepBSONData)
}

func BSONDeepDocumentDecodingLazy(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentDecodingLazy(ctx, tm, iters, deepBSONData)
}

func BSONDeepDocumentDecoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentDecoding(ctx, tm, iters, 126, deepBSONData)
}

func BSONFullDocumentEncoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentEncoding(ctx, tm, iters, fullBSONData)
}

func BSONFullDocumentDecodingLazy(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentDecodingLazy(ctx, tm, iters, fullBSONData)
}

func BSONFullDocumentDecoding(ctx context.Context, tm TimerManager, iters int) error {
	return bsonDocumentDecoding(ctx, tm, iters, 145, fullBSONData)
}
