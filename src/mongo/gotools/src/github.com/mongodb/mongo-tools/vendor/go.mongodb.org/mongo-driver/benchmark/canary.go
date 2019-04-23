// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"context"
)

func CanaryIncCase(ctx context.Context, tm TimerManager, iters int) error {
	var canaryCount int
	for i := 0; i < iters; i++ {
		canaryCount++
	}
	return nil
}

var globalCanaryCount int

func GlobalCanaryIncCase(ctx context.Context, tm TimerManager, iters int) error {
	for i := 0; i < iters; i++ {
		globalCanaryCount++
	}

	return nil
}
