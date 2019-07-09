// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package topology

import (
	"container/list"
	"sync"
	"time"
)

// expiredFunc is the function type used for testing whether or not resources in a resourcePool have expired. It should
// return true if the resource has expired and can be removed from the pool.
type expiredFunc func(interface{}) bool

// closeFunc is the function type used to close resources in a resourcePool. The pool will always call this function
// asynchronously.
type closeFunc func(interface{})

// resourcePool is a concurrent resource pool that implements the behavior described in the sessions spec.
type resourcePool struct {
	deque         *list.List
	len, maxSize  uint64
	expiredFn     expiredFunc
	closeFn       closeFunc
	pruneTimer    *time.Timer
	pruneInterval time.Duration

	sync.Mutex
}

// NewResourcePool creates a new resourcePool instance that is capped to maxSize resources.
// If maxSize is 0, the pool size will be unbounded.
func newResourcePool(maxSize uint64, expiredFn expiredFunc, closeFn closeFunc, pruneInterval time.Duration) *resourcePool {
	rp := &resourcePool{
		deque:         list.New(),
		maxSize:       maxSize,
		expiredFn:     expiredFn,
		closeFn:       closeFn,
		pruneInterval: pruneInterval,
	}
	rp.Lock()
	rp.pruneTimer = time.AfterFunc(rp.pruneInterval, rp.Prune)
	rp.Unlock()
	return rp
}

// Get returns the first un-expired resource from the pool. If no such resource can be found, nil is returned.
func (rp *resourcePool) Get() interface{} {
	rp.Lock()
	defer rp.Unlock()

	var next *list.Element
	for curr := rp.deque.Front(); curr != nil; curr = next {
		next = curr.Next()

		// remove the current resource and return it if it is valid
		rp.deque.Remove(curr)
		rp.len--
		if !rp.expiredFn(curr.Value) {
			// found un-expired resource
			return curr.Value
		}

		// close expired resources
		rp.closeFn(curr.Value)
	}

	// did not find a valid resource
	return nil
}

// Put clears expired resources from the pool and then returns resource v to the pool if there is room. It returns true
// if v was successfully added to the pool and false otherwise.
func (rp *resourcePool) Put(v interface{}) bool {
	rp.Lock()
	defer rp.Unlock()

	// close expired resources from the back of the pool
	rp.prune()
	if (rp.maxSize != 0 && rp.len == rp.maxSize) || rp.expiredFn(v) {
		return false
	}
	rp.deque.PushFront(v)
	rp.len++
	return true
}

// Prune clears expired resources from the pool.
func (rp *resourcePool) Prune() {
	rp.Lock()
	defer rp.Unlock()
	rp.prune()
}

func (rp *resourcePool) prune() {
	// iterate over the list and stop at the first valid value
	var prev *list.Element
	for curr := rp.deque.Back(); curr != nil; curr = prev {
		prev = curr.Prev()
		if !rp.expiredFn(curr.Value) {
			// found unexpired resource
			break
		}

		// remove and close expired resources
		rp.deque.Remove(curr)
		rp.closeFn(curr.Value)
		rp.len--
	}

	// reset the timer for the background cleanup routine
	if !rp.pruneTimer.Stop() {
		rp.pruneTimer = time.AfterFunc(rp.pruneInterval, rp.Prune)
		return
	}
	rp.pruneTimer.Reset(rp.pruneInterval)
}
