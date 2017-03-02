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

// countProgressor is an implementation of Progressor that uses
type countProgressor struct {
	max, current int64
}

func (c *countProgressor) Progress() (int64, int64) {
	current := atomic.LoadInt64(&c.current)
	return current, c.max
}

func (c *countProgressor) Inc(amount int64) {
	atomic.AddInt64(&c.current, amount)
}

func (c *countProgressor) Set(amount int64) {
	atomic.StoreInt64(&c.current, amount)
}

func NewCounter(max int64) *countProgressor {
	return &countProgressor{max, 0}
}
