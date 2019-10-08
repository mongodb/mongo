// Copyright (C) MongoDB, Inc. 2019-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package txn implements functions for examining and processing transaction
// oplog entries.
package txn

import (
	"errors"
	"fmt"
	"sync"

	"github.com/mongodb/mongo-tools-common/bsonutil"
	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/util"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
)

var ErrBufferClosed = errors.New("transaction buffer already closed")
var ErrTxnAborted = errors.New("transaction aborted")
var ErrNotTransaction = errors.New("oplog entry is not a transaction")

var zeroTimestamp = primitive.Timestamp{}

type txnTask struct {
	meta Meta
	op   db.Oplog
}

// txnState tracks an individual transaction, including storage of related ops
// and communication channels.  It includes a WaitGroup for waiting on
// transaction-related goroutines.
type txnState struct {
	buffer     []db.Oplog
	ingestChan chan txnTask
	ingestDone chan struct{}
	ingestErr  error
	stopChan   chan struct{}
	startTime  primitive.Timestamp
	wg         sync.WaitGroup
}

func newTxnState(op db.Oplog) *txnState {
	return &txnState{
		ingestChan: make(chan txnTask),
		ingestDone: make(chan struct{}),
		stopChan:   make(chan struct{}),
		buffer:     make([]db.Oplog, 0),
		startTime:  op.Timestamp,
	}
}

// Because state is currently kept in memory, purge merely drops the reference
// so the GC will eventually clean up.  Eventually, this might clean up a file
// on disk.
func (ts *txnState) purge() error {
	ts.buffer = nil
	return nil
}

// Buffer stores transaction oplog entries until they are needed
// to commit them to a desination.  It includes a WaitGroup for tracking
// all goroutines across all transactions for use in global shutdown.
type Buffer struct {
	sync.Mutex
	stopped bool
	txns    map[ID]*txnState
	wg      sync.WaitGroup
}

// NewBuffer initializes a transaction oplog buffer.
func NewBuffer() *Buffer {
	return &Buffer{
		txns: make(map[ID]*txnState),
	}
}

// Concurrency notes:
//
// We require that AddOp, GetTxnStream and PurgeTxn be called serially as part
// of orchestrating replay of oplog entries.  The only method that could run
// concurrently is Stop. If Stop is called, we're in some sort of global
// shutdown, so we don't care how other methods and goroutines resolve, only
// that they do so without panicking.
//

// AddOp sends a transaction oplog entry to a background goroutine (starting
// one for a new transaction ID) for asynchronous pre-processing and storage.
// If the oplog entry is not a transaction, an error will be returned.  Any
// errors during processing can be discovered later via the error channel from
// `GetTxnStream`.
//
// Must not be called concurrently with other transaction-related operations.
// Must not be called for a given transaction after starting to stream that
// transaction.
func (b *Buffer) AddOp(m Meta, op db.Oplog) error {
	b.Lock()
	defer b.Unlock()

	if b.stopped {
		return ErrBufferClosed
	}

	if !m.IsTxn() {
		return ErrNotTransaction
	}

	// Get or initialize transaction state
	state, ok := b.txns[m.id]
	if !ok {
		state = newTxnState(op)
		b.txns[m.id] = state
		b.wg.Add(1)
		state.wg.Add(1)
		go b.ingester(state)
	}

	// Send unless the ingester has shut down, e.g. on error
	select {
	case <-state.ingestDone:
	case state.ingestChan <- txnTask{meta: m, op: op}:
	}

	return nil
}

func (b *Buffer) ingester(state *txnState) {
LOOP:
	for {
		select {
		case t := <-state.ingestChan:
			if t.meta.IsData() {
				// process it
				innerOps, err := extractInnerOps(t.op.Object)
				if err != nil {
					state.ingestErr = err
					break LOOP
				}
				// store it
				for _, op := range innerOps {
					state.buffer = append(state.buffer, op)
				}
			}
			if t.meta.IsFinal() {
				break LOOP
			}
		case <-state.stopChan:
			break LOOP
		}
	}
	close(state.ingestDone)
	state.wg.Done()
	b.wg.Done()
}

// GetTxnStream returns a channel of Oplog entries in a transaction and a
// channel for errors.  If the buffer has been stopped, the returned op channel
// will be closed and the error channel will have an error on it.
//
// Must not be called concurrently with other transaction-related operations.
// For a given transaction, it must not be called until after a final oplog
// entry has been passed to AddOp and it must not be called more than once.
func (b *Buffer) GetTxnStream(m Meta) (<-chan db.Oplog, <-chan error) {
	b.Lock()
	defer b.Unlock()

	opChan := make(chan db.Oplog)
	errChan := make(chan error, 1)

	if b.stopped {
		return sendErrAndClose(opChan, errChan, ErrBufferClosed)
	}

	if !m.IsTxn() {
		return sendErrAndClose(opChan, errChan, ErrNotTransaction)
	}

	state := b.txns[m.id]
	if state == nil {
		return sendErrAndClose(opChan, errChan, fmt.Errorf("GetTxnStream found no state for %v", m.id))
	}

	// The final oplog entry must have been passed to AddOp before calling this
	// method, so we know this will be able to make progress.
	<-state.ingestDone

	if state.ingestErr != nil {
		return sendErrAndClose(opChan, errChan, state.ingestErr)
	}

	// Launch streaming goroutine
	b.wg.Add(1)
	state.wg.Add(1)
	go b.streamer(state, opChan, errChan)

	return opChan, errChan
}

func (b *Buffer) streamer(state *txnState, opChan chan<- db.Oplog, errChan chan<- error) {
LOOP:
	for _, op := range state.buffer {
		select {
		case opChan <- op:
		case <-state.stopChan:
			errChan <- ErrTxnAborted
			break LOOP
		}
	}
	close(opChan)
	close(errChan)
	state.wg.Done()
	b.wg.Done()
}

// OldestTimestamp returns the timestamp of the oldest buffered transaction, or
// a zero-value timestamp if no transactions are buffered.  This will include
// committed transactions until they are purged.
func (b *Buffer) OldestTimestamp() primitive.Timestamp {
	b.Lock()
	defer b.Unlock()
	oldest := zeroTimestamp
	for _, v := range b.txns {
		if oldest == zeroTimestamp || util.TimestampLessThan(v.startTime, oldest) {
			oldest = v.startTime
		}
	}
	return oldest
}

// PurgeTxn closes any transaction streams in progress and deletes all oplog
// entries associated with a transaction.
//
// Must not be called concurrently with other transaction-related operations.
// For a given transaction, it must not be called until after a final oplog
// entry has been passed to AddOp and it must not be called more than once.
func (b *Buffer) PurgeTxn(m Meta) error {
	b.Lock()
	defer b.Unlock()
	if b.stopped {
		return ErrBufferClosed
	}
	state := b.txns[m.id]
	if state == nil {
		return fmt.Errorf("PurgeTxn found no state for %v", m.id)
	}

	// When the lock is dropped, we don't want Stop to find this transaction and
	// double-close it.
	delete(b.txns, m.id)
	close(state.stopChan)

	// Wait for goroutines to terminate, then clean up.
	state.wg.Wait()
	state.purge()

	return nil
}

// Stop shuts down processing and cleans up.  Subsequent calls to Stop() will return nil.
// All other methods error after this is called.
func (b *Buffer) Stop() error {
	b.Lock()
	if b.stopped {
		b.Unlock()
		return nil
	}

	b.stopped = true
	for _, state := range b.txns {
		close(state.stopChan)
	}

	b.Unlock()

	// At this point we know any subsequent public method will see the buffer
	// is stopped, no new goroutines will be launched, and existing goroutines
	// have been signaled to close.  Next, wait for goroutines to stop, then
	// clean up.

	b.wg.Wait()
	var firstErr error
	for _, state := range b.txns {
		err := state.purge()
		if err != nil && firstErr == nil {
			firstErr = err
		}
	}

	return firstErr
}

// sendErrAndClose is a utility for putting an error on a channel before closing.
func sendErrAndClose(o chan db.Oplog, e chan error, err error) (chan db.Oplog, chan error) {
	e <- err
	close(o)
	close(e)
	return o, e
}

const extractErrorFmt = "error extracting transaction ops: %s: %v"

func extractInnerOps(doc bson.D) ([]db.Oplog, error) {
	rawAO, err := bsonutil.FindValueByKey("applyOps", &doc)
	if err != nil {
		return nil, fmt.Errorf(extractErrorFmt, "applyOps field", err)
	}

	ao, ok := rawAO.(bson.A)
	if !ok {
		return nil, fmt.Errorf(extractErrorFmt, "applyOps field", "not a BSON array")
	}

	ops := make([]db.Oplog, len(ao))
	for i, v := range ao {
		opDoc, ok := v.(bson.D)
		if !ok {
			return nil, fmt.Errorf(extractErrorFmt, "applyOps op", "not a BSON document")
		}
		op, err := bsonDocToOplog(opDoc)
		if err != nil {
			return nil, fmt.Errorf(extractErrorFmt, "applyOps op", err)
		}
		ops[i] = *op
	}

	return ops, nil
}

const opConvertErrorFmt = "error converting bson.D to op: %s: %v"

func bsonDocToOplog(doc bson.D) (*db.Oplog, error) {
	op := db.Oplog{}

	for _, v := range doc {
		switch v.Key {
		case "op":
			s, ok := v.Value.(string)
			if !ok {
				return nil, fmt.Errorf(opConvertErrorFmt, "op field", "not a string")
			}
			op.Operation = s
		case "ns":
			s, ok := v.Value.(string)
			if !ok {
				return nil, fmt.Errorf(opConvertErrorFmt, "ns field", "not a string")
			}
			op.Namespace = s
		case "o":
			d, ok := v.Value.(bson.D)
			if !ok {
				return nil, fmt.Errorf(opConvertErrorFmt, "o field", "not a BSON Document")
			}
			op.Object = d
		case "o2":
			d, ok := v.Value.(bson.D)
			if !ok {
				return nil, fmt.Errorf(opConvertErrorFmt, "o2 field", "not a BSON Document")
			}
			op.Query = d
		case "ui":
			u, ok := v.Value.(primitive.Binary)
			if !ok {
				return nil, fmt.Errorf(opConvertErrorFmt, "ui field", "not binary data")
			}
			op.UI = &u
		}
	}

	return &op, nil
}
