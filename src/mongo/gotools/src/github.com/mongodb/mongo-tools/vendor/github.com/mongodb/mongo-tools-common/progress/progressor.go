// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package progress exposes utilities to asynchronously monitor and display processing progress.
package progress

import (
	"sync/atomic"
)

// Progressor can be implemented to allow an object to hook up to a progress.Bar.
type Progressor interface {
	// Progress returns a pair of integers: the amount completed and the total
	// amount to reach 100%. This method is called by progress.Bar to determine
	// what percentage to display.
	Progress() (current, max int64)
}

// Updateable is a Progressor which also exposes the ability for the progressing
// value to be updated.
type Updateable interface {
	Progressor

	// Inc increments the current progress counter by the given amount.
	Inc(amount int64)

	// Set sets the progress counter to the given amount.
	Set(amount int64)
}

// CountProgressor is an implementation of Progressor that uses
type CountProgressor struct {
	max, current int64
}

// Progress returns the current and maximum values of the counter.
func (c *CountProgressor) Progress() (int64, int64) {
	current := atomic.LoadInt64(&c.current)
	return current, c.max
}

// Inc atomically increments the counter by the given amount.
func (c *CountProgressor) Inc(amount int64) {
	atomic.AddInt64(&c.current, amount)
}

// Set atomically sets the counter to a given number.
func (c *CountProgressor) Set(amount int64) {
	atomic.StoreInt64(&c.current, amount)
}

// NewCounter constructs a CountProgressor with a given maximum count.
func NewCounter(max int64) *CountProgressor {
	return &CountProgressor{max, 0}
}
