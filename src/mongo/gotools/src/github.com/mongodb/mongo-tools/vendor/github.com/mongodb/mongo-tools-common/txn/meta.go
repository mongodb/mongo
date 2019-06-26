// Copyright (C) MongoDB, Inc. 2019-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package txn

import (
	"encoding/base64"
	"fmt"

	"github.com/mongodb/mongo-tools-common/db"
)

// "empty" prevOpTime is {ts: Timestamp(0, 0), t: NumberLong(-1)} as BSON.
var emptyPrev = string([]byte{
	28, 0, 0, 0, 17, 116, 115, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	18, 116, 0, 255, 255, 255, 255, 255, 255, 255, 255, 0,
})

// ID wraps fields needed to uniquely identify a transaction for use as a map
// key.  The 'lsid' is a string rather than bson.Raw or []byte so that this
// type is a valid map key.
type ID struct {
	lsid      string
	txnNumber int64
}

func (id ID) String() string {
	return fmt.Sprintf("%s-%d", base64.RawStdEncoding.EncodeToString([]byte(id.lsid)), id.txnNumber)
}

// Meta holds information extracted from an oplog entry for later routing
// logic.  Zero value means 'not a transaction'.  We store 'prevOpTime' as
// string so the struct is comparable.
type Meta struct {
	id         ID
	commit     bool
	abort      bool
	partial    bool
	prepare    bool
	prevOpTime string
}

// NewMeta extracts transaction metadata from an oplog entry.  A
// non-transaction will return a zero-value Meta struct, not an error.
//
// Currently there is no way for this to error, but that may change in the
// future if we change the db.Oplog.Object to bson.Raw, so the API is designed
// with failure as a possibility.
func NewMeta(op db.Oplog) (Meta, error) {
	if op.LSID == nil {
		return Meta{}, nil
	}

	// Default prevOpTime to empty to "upgrade" 4.0 transactions without it.
	m := Meta{
		id:         ID{lsid: string(op.LSID), txnNumber: *op.TxnNumber},
		prevOpTime: emptyPrev,
	}

	if op.PrevOpTime != nil {
		m.prevOpTime = string(op.PrevOpTime)
	}

	for _, e := range op.Object {
		switch e.Key {
		case "commitTransaction":
			m.commit = true
		case "abortTransaction":
			m.abort = true
		case "partialTxn":
			m.partial = true
		case "prepare":
			m.prepare = true
		}
	}

	return m, nil
}

// IsAbort is true if the oplog entry had the abort command.
func (m Meta) IsAbort() bool {
	return m.abort
}

// IsData is true if the oplog entry contains transaction data
func (m Meta) IsData() bool {
	return !m.commit && !m.abort
}

// IsCommit is true if the oplog entry was an abort command or was the
// final entry of an unprepared transaction.
func (m Meta) IsCommit() bool {
	return m.commit || (m.IsTxn() && !m.prepare && !m.partial)
}

// IsFinal is true if the oplog entry is the closing entry of a transaction,
// i.e. if IsAbort or IsCommit is true.
func (m Meta) IsFinal() bool {
	return m.IsCommit() || m.IsAbort()
}

// IsMultiOp is true if the oplog entry is part of a prepared and/or large
// transaction.
func (m Meta) IsMultiOp() bool {
	return m.partial || m.prepare || (m.IsTxn() && m.prevOpTime != emptyPrev)
}

// IsTxn is true if the oplog entry is part of any transaction, i.e. the lsid field
// exists.
func (m Meta) IsTxn() bool {
	return m != Meta{}
}
