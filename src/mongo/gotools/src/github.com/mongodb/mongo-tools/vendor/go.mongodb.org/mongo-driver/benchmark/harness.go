// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark // import "go.mongodb.org/mongo-driver/benchmark"

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

const (
	five            = 5
	ten             = 2 * five
	hundred         = ten * ten
	thousand        = ten * hundred
	tenThousand     = ten * thousand
	hundredThousand = hundred * thousand
	million         = hundred * hundredThousand
	halfMillion     = five * hundredThousand

	ExecutionTimeout = five * time.Minute
	StandardRuntime  = time.Minute
	MinimumRuntime   = five * time.Second
	MinIterations    = hundred
)

type BenchCase func(context.Context, TimerManager, int) error
type BenchFunction func(*testing.B)

func WrapCase(bench BenchCase) BenchFunction {
	name := getName(bench)
	return func(b *testing.B) {
		ctx := context.Background()
		b.ResetTimer()
		err := bench(ctx, b, b.N)
		require.NoError(b, err, "case='%s'", name)
	}
}

func getAllCases() []*CaseDefinition {
	return []*CaseDefinition{
		{
			Bench:              CanaryIncCase,
			Count:              million,
			Size:               -1,
			Runtime:            MinimumRuntime,
			RequiredIterations: ten,
		},
		{
			Bench:              GlobalCanaryIncCase,
			Count:              million,
			Size:               -1,
			Runtime:            MinimumRuntime,
			RequiredIterations: ten,
		},
		{
			Bench:   BSONFlatDocumentEncoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONFlatDocumentDecodingLazy,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONFlatDocumentDecoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONDeepDocumentEncoding,
			Count:   tenThousand,
			Size:    19640000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONDeepDocumentDecodingLazy,
			Count:   tenThousand,
			Size:    19640000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONDeepDocumentDecoding,
			Count:   tenThousand,
			Size:    19640000,
			Runtime: StandardRuntime,
		},
		// {
		//	Bench:   BSONFullDocumentEncoding,
		//	Count:   tenThousand,
		//	Size:    57340000,
		//	Runtime: StandardRuntime,
		// },
		// {
		//	Bench:   BSONFullDocumentDecodingLazy,
		//	Count:   tenThousand,
		//	Size:    57340000,
		//	Runtime: StandardRuntime,
		// },
		// {
		//	Bench:   BSONFullDocumentDecoding,
		//	Count:   tenThousand,
		//	Size:    57340000,
		//	Runtime: StandardRuntime,
		// },
		// {
		//	Bench:   BSONFullReaderDecoding,
		//	Count:   tenThousand,
		//	Size:    57340000,
		//	Runtime: StandardRuntime,
		// },
		{
			Bench:   BSONFlatMapDecoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONFlatMapEncoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONDeepMapDecoding,
			Count:   tenThousand,
			Size:    19640000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONDeepMapEncoding,
			Count:   tenThousand,
			Size:    19640000,
			Runtime: StandardRuntime,
		},
		// {
		//	Bench:   BSONFullMapDecoding,
		//	Count:   tenThousand,
		//	Size:    57340000,
		//	Runtime: StandardRuntime,
		// },
		// {
		//	Bench:   BSONFullMapEncoding,
		//	Count:   tenThousand,
		//	Size:    57340000,
		//	Runtime: StandardRuntime,
		// },
		{
			Bench:   BSONFlatStructDecoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONFlatStructTagsDecoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONFlatStructEncoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   BSONFlatStructTagsEncoding,
			Count:   tenThousand,
			Size:    75310000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   SingleRunCommand,
			Count:   tenThousand,
			Size:    160000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   SingleFindOneByID,
			Count:   tenThousand,
			Size:    16220000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   SingleInsertSmallDocument,
			Count:   tenThousand,
			Size:    2750000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   SingleInsertLargeDocument,
			Count:   ten,
			Size:    27310890,
			Runtime: StandardRuntime,
		},
		{
			Bench:   MultiFindMany,
			Count:   tenThousand,
			Size:    16220000,
			Runtime: StandardRuntime,
		},
		{
			Bench:   MultiInsertSmallDocument,
			Count:   tenThousand,
			Size:    2750000,
			Runtime: StandardRuntime,
		},
		{
			Bench:              MultiInsertLargeDocument,
			Count:              ten,
			Size:               27310890,
			Runtime:            StandardRuntime,
			RequiredIterations: tenThousand,
		},
	}
}
