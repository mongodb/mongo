package mgo

import (
	"bytes"

	"github.com/10gen/llmgo/bson"
)

// Bulk represents an operation that can be prepared with several
// orthogonal changes before being delivered to the server.
//
// Relevant documentation:
//
//   http://blog.mongodb.org/post/84922794768/mongodbs-new-bulk-api
//
type Bulk struct {
	c       *Collection
	ordered bool
	actions []bulkAction
}

type bulkOp int

const (
	bulkInsert bulkOp = iota + 1
	bulkUpdate
	bulkUpdateAll
	bulkRemove
)

type bulkAction struct {
	op   bulkOp
	docs []interface{}
}

type bulkUpdateOp []interface{}
type bulkDeleteOp []interface{}

// BulkError holds an error returned from running a Bulk operation.
//
// TODO: This is private for the moment, until we understand exactly how
//       to report these multi-errors in a useful and convenient way.
type bulkError struct {
	errs []error
}

// BulkResult holds the results for a bulk operation.
type BulkResult struct {
	Matched  int
	Modified int // Available only for MongoDB 2.6+

	// Be conservative while we understand exactly how to report these
	// results in a useful and convenient way, and also how to emulate
	// them with prior servers.
	private bool
}

func (e *bulkError) Error() string {
	if len(e.errs) == 0 {
		return "invalid bulkError instance: no errors"
	}
	if len(e.errs) == 1 {
		return e.errs[0].Error()
	}
	msgs := make([]string, 0, len(e.errs))
	seen := make(map[string]bool)
	for _, err := range e.errs {
		msg := err.Error()
		if !seen[msg] {
			seen[msg] = true
			msgs = append(msgs, msg)
		}
	}
	if len(msgs) == 1 {
		return msgs[0]
	}
	var buf bytes.Buffer
	buf.WriteString("multiple errors in bulk operation:\n")
	for _, msg := range msgs {
		buf.WriteString("  - ")
		buf.WriteString(msg)
		buf.WriteByte('\n')
	}
	return buf.String()
}

// Bulk returns a value to prepare the execution of a bulk operation.
func (c *Collection) Bulk() *Bulk {
	return &Bulk{c: c, ordered: true}
}

// Unordered puts the bulk operation in unordered mode.
//
// In unordered mode the indvidual operations may be sent
// out of order, which means latter operations may proceed
// even if prior ones have failed.
func (b *Bulk) Unordered() {
	b.ordered = false
}

func (b *Bulk) action(op bulkOp) *bulkAction {
	if len(b.actions) > 0 && b.actions[len(b.actions)-1].op == op {
		return &b.actions[len(b.actions)-1]
	}
	if !b.ordered {
		for i := range b.actions {
			if b.actions[i].op == op {
				return &b.actions[i]
			}
		}
	}
	b.actions = append(b.actions, bulkAction{op: op})
	return &b.actions[len(b.actions)-1]
}

// Insert queues up the provided documents for insertion.
func (b *Bulk) Insert(docs ...interface{}) {
	action := b.action(bulkInsert)
	action.docs = append(action.docs, docs...)
}

// Remove queues up the provided selectors for removing matching documents.
// Each selector will remove only a single matching document.
func (b *Bulk) Remove(selectors ...interface{}) {
	action := b.action(bulkRemove)
	for _, selector := range selectors {
		if selector == nil {
			selector = bson.D{}
		}
		action.docs = append(action.docs, &DeleteOp{
			Collection: b.c.FullName,
			Selector:   selector,
			Flags:      1,
			Limit:      1,
		})
	}
}

// RemoveAll queues up the provided selectors for removing all matching documents.
// Each selector will remove all matching documents.
func (b *Bulk) RemoveAll(selectors ...interface{}) {
	action := b.action(bulkRemove)
	for _, selector := range selectors {
		if selector == nil {
			selector = bson.D{}
		}
		action.docs = append(action.docs, &DeleteOp{
			Collection: b.c.FullName,
			Selector:   selector,
			Flags:      0,
			Limit:      0,
		})
	}
}

// Update queues up the provided pairs of updating instructions.
// The first element of each pair selects which documents must be
// updated, and the second element defines how to update it.
// Each pair matches exactly one document for updating at most.
func (b *Bulk) Update(pairs ...interface{}) {
	if len(pairs)%2 != 0 {
		panic("Bulk.Update requires an even number of parameters")
	}
	action := b.action(bulkUpdate)
	for i := 0; i < len(pairs); i += 2 {
		selector := pairs[i]
		if selector == nil {
			selector = bson.D{}
		}
		action.docs = append(action.docs, &UpdateOp{
			Collection: b.c.FullName,
			Selector:   selector,
			Update:     pairs[i+1],
		})
	}
}

// UpdateAll queues up the provided pairs of updating instructions.
// The first element of each pair selects which documents must be
// updated, and the second element defines how to update it.
// Each pair updates all documents matching the selector.
func (b *Bulk) UpdateAll(pairs ...interface{}) {
	if len(pairs)%2 != 0 {
		panic("Bulk.UpdateAll requires an even number of parameters")
	}
	action := b.action(bulkUpdate)
	for i := 0; i < len(pairs); i += 2 {
		selector := pairs[i]
		if selector == nil {
			selector = bson.D{}
		}
		action.docs = append(action.docs, &UpdateOp{
			Collection: b.c.FullName,
			Selector:   selector,
			Update:     pairs[i+1],
			Flags:      2,
			Multi:      true,
		})
	}
}

// Upsert queues up the provided pairs of upserting instructions.
// The first element of each pair selects which documents must be
// updated, and the second element defines how to update it.
// Each pair matches exactly one document for updating at most.
func (b *Bulk) Upsert(pairs ...interface{}) {
	if len(pairs)%2 != 0 {
		panic("Bulk.Update requires an even number of parameters")
	}
	action := b.action(bulkUpdate)
	for i := 0; i < len(pairs); i += 2 {
		selector := pairs[i]
		if selector == nil {
			selector = bson.D{}
		}
		action.docs = append(action.docs, &UpdateOp{
			Collection: b.c.FullName,
			Selector:   selector,
			Update:     pairs[i+1],
			Flags:      1,
			Upsert:     true,
		})
	}
}

// Run runs all the operations queued up.
//
// If an error is reported on an unordered bulk operation, the error value may
// be an aggregation of all issues observed. As an exception to that, Insert
// operations running on MongoDB versions prior to 2.6 will report the last
// error only due to a limitation in the wire protocol.
func (b *Bulk) Run() (*BulkResult, error) {
	var result BulkResult
	var berr bulkError
	var failed bool
	for i := range b.actions {
		action := &b.actions[i]
		var ok bool
		switch action.op {
		case bulkInsert:
			ok = b.runInsert(action, &result, &berr)
		case bulkUpdate:
			ok = b.runUpdate(action, &result, &berr)
		case bulkRemove:
			ok = b.runRemove(action, &result, &berr)
		default:
			panic("unknown bulk operation")
		}
		if !ok {
			failed = true
			if b.ordered {
				break
			}
		}
	}
	if failed {
		return nil, &berr
	}
	return &result, nil
}

func (b *Bulk) runInsert(action *bulkAction, result *BulkResult, berr *bulkError) bool {
	op := &InsertOp{b.c.FullName, action.docs, 0}
	if !b.ordered {
		op.Flags = 1 // ContinueOnError
	}
	lerr, err := b.c.writeOp(op, b.ordered)
	return b.checkSuccess(berr, lerr, err)
}

func (b *Bulk) runUpdate(action *bulkAction, result *BulkResult, berr *bulkError) bool {
	lerr, err := b.c.writeOp(bulkUpdateOp(action.docs), b.ordered)
	if lerr != nil {
		result.Matched += lerr.N
		result.Modified += lerr.modified
	}
	return b.checkSuccess(berr, lerr, err)
}

func (b *Bulk) runRemove(action *bulkAction, result *BulkResult, berr *bulkError) bool {
	lerr, err := b.c.writeOp(bulkDeleteOp(action.docs), b.ordered)
	if lerr != nil {
		result.Matched += lerr.N
		result.Modified += lerr.modified
	}
	return b.checkSuccess(berr, lerr, err)
}

func (b *Bulk) checkSuccess(berr *bulkError, lerr *LastError, err error) bool {
	if lerr != nil && len(lerr.errors) > 0 {
		berr.errs = append(berr.errs, lerr.errors...)
		return false
	} else if err != nil {
		berr.errs = append(berr.errs, err)
		return false
	}
	return true
}
