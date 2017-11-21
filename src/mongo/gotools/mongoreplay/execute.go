// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"sync"
	"time"

	mgo "github.com/10gen/llmgo"
	"github.com/patrickmn/go-cache"
)

// ReplyPair contains both a live reply and a recorded reply when fully
// occupied.
type ReplyPair struct {
	ops [2]Replyable
}

const (
	// ReplyFromWire is the ReplyPair index for live replies.
	ReplyFromWire = 0
	// ReplyFromFile is the ReplyPair index for recorded replies.
	ReplyFromFile = 1
)

// ExecutionContext maintains information for a mongoreplay execution.
type ExecutionContext struct {
	// IncompleteReplies holds half complete ReplyPairs, which contains either a
	// live reply or a recorded reply when one arrives before the other.
	IncompleteReplies *cache.Cache

	// CompleteReplies contains ReplyPairs that have been competed by the
	// arrival of the missing half of.
	CompleteReplies map[string]*ReplyPair

	// CursorIDMap contains the mapping between recorded cursorIDs and live
	// cursorIDs
	CursorIDMap cursorManager

	// lock synchronizes access to all of the caches and maps in the
	// ExecutionContext
	sync.Mutex

	ConnectionChansWaitGroup sync.WaitGroup

	*StatCollector

	// fullSpeed is a control to indicate whether the tool will sleep to synchronize
	// the playback of operations or if it will play back all operations as fast
	// as possible.
	fullSpeed bool

	driverOpsFiltered bool

	session *mgo.Session
}

// ExecutionOptions holds the additional configuration options needed to completely
// create an execution session.
type ExecutionOptions struct {
	fullSpeed         bool
	driverOpsFiltered bool
}

// NewExecutionContext initializes a new ExecutionContext.
func NewExecutionContext(statColl *StatCollector, session *mgo.Session, options *ExecutionOptions) *ExecutionContext {
	return &ExecutionContext{
		IncompleteReplies: cache.New(60*time.Second, 60*time.Second),
		CompleteReplies:   map[string]*ReplyPair{},
		CursorIDMap:       newCursorCache(),
		StatCollector:     statColl,
		fullSpeed:         options.fullSpeed,
		driverOpsFiltered: options.driverOpsFiltered,
		session:           session,
	}
}

// AddFromWire adds a from-wire reply to its IncompleteReplies ReplyPair and
// moves that ReplyPair to CompleteReplies if it's complete.  The index is based
// on the src/dest of the recordedOp which should be the op that this ReplyOp is
// a reply to.
func (context *ExecutionContext) AddFromWire(reply Replyable, recordedOp *RecordedOp) {
	if cursorID, _ := reply.getCursorID(); cursorID == 0 {
		return
	}
	key := cacheKey(recordedOp, false)
	toolDebugLogger.Logvf(DebugHigh, "Adding live reply with key %v", key)
	context.completeReply(key, reply, ReplyFromWire)
}

// AddFromFile adds a from-file reply to its IncompleteReplies ReplyPair and
// moves that ReplyPair to CompleteReplies if it's complete.  The index is based
// on the reversed src/dest of the recordedOp which should the RecordedOp that
// this ReplyOp was unmarshaled out of.
func (context *ExecutionContext) AddFromFile(reply Replyable, recordedOp *RecordedOp) {
	if cursorID, _ := reply.getCursorID(); cursorID == 0 {
		return
	}
	key := cacheKey(recordedOp, true)
	toolDebugLogger.Logvf(DebugHigh, "Adding recorded reply with key %v", key)
	context.completeReply(key, reply, ReplyFromFile)
}

func (context *ExecutionContext) completeReply(key string, reply Replyable, opSource int) {
	context.Lock()
	if cacheValue, ok := context.IncompleteReplies.Get(key); !ok {
		rp := &ReplyPair{}
		rp.ops[opSource] = reply
		context.IncompleteReplies.Set(key, rp, cache.DefaultExpiration)
	} else {
		rp := cacheValue.(*ReplyPair)
		rp.ops[opSource] = reply
		if rp.ops[1-opSource] != nil {
			context.CompleteReplies[key] = rp
			context.IncompleteReplies.Delete(key)
		}
	}
	context.Unlock()
}

func (context *ExecutionContext) rewriteCursors(rewriteable cursorsRewriteable, connectionNum int64) (bool, error) {
	cursorIDs, err := rewriteable.getCursorIDs()

	index := 0
	for _, cursorID := range cursorIDs {
		userInfoLogger.Logvf(DebugLow, "Rewriting cursorID : %v", cursorID)
		liveCursorID, ok := context.CursorIDMap.GetCursor(cursorID, connectionNum)
		if ok {
			cursorIDs[index] = liveCursorID
			index++
		} else {
			userInfoLogger.Logvf(DebugLow, "Missing mapped cursorID for raw cursorID : %v", cursorID)
		}
	}
	newCursors := cursorIDs[0:index]
	err = rewriteable.setCursorIDs(newCursors)
	if err != nil {
		return false, err
	}
	return len(newCursors) != 0, nil
}

func (context *ExecutionContext) handleCompletedReplies() error {
	context.Lock()
	for key, rp := range context.CompleteReplies {
		userInfoLogger.Logvf(DebugHigh, "Completed reply: %v, %v", rp.ops[ReplyFromFile], rp.ops[ReplyFromWire])
		cursorFromFile, err := rp.ops[ReplyFromFile].getCursorID()
		if err != nil {
			return err
		}
		cursorFromWire, err := rp.ops[ReplyFromWire].getCursorID()
		if err != nil {
			return err
		}
		if cursorFromFile != 0 {
			context.CursorIDMap.SetCursor(cursorFromFile, cursorFromWire)
		}

		delete(context.CompleteReplies, key)
	}

	context.Unlock()
	return nil
}

func (context *ExecutionContext) newExecutionConnection(start time.Time, connectionNum int64) chan<- *RecordedOp {
	ch := make(chan *RecordedOp, 10000)
	context.ConnectionChansWaitGroup.Add(1)

	go func() {
		now := time.Now()
		var connected bool
		time.Sleep(start.Add(-5 * time.Second).Sub(now)) // Sleep until five seconds before the start time
		socket, err := context.session.AcquireSocketDirect()
		if err == nil {
			userInfoLogger.Logvf(Info, "(Connection %v) New connection CREATED.", connectionNum)
			connected = true
			defer socket.Close()
		} else {
			userInfoLogger.Logvf(Info, "(Connection %v) New Connection FAILED: %v", connectionNum, err)
		}
		for recordedOp := range ch {
			var parsedOp Op
			var reply Replyable
			var err error
			msg := ""
			if connected {
				// Populate the op with the connection num it's being played on.
				// This allows it to be used for downstream reporting of stats.
				recordedOp.PlayedConnectionNum = connectionNum
				t := time.Now()

				if !context.fullSpeed && recordedOp.RawOp.Header.OpCode != OpCodeReply {
					if t.Before(recordedOp.PlayAt.Time) {
						time.Sleep(recordedOp.PlayAt.Sub(t))
					}
				}
				userInfoLogger.Logvf(DebugHigh, "(Connection %v) op %v", connectionNum, recordedOp.String())
				parsedOp, reply, err = context.Execute(recordedOp, socket)
				if err != nil {
					toolDebugLogger.Logvf(Always, "context.Execute error: %v", err)
				}
			} else {
				parsedOp, err = recordedOp.Parse()
				if err != nil {
					toolDebugLogger.Logvf(Always, "Execution Connection error: %v", err)
				}

				msg = fmt.Sprintf("Skipped on non-connected socket (Connection %v)", connectionNum)
				toolDebugLogger.Logv(Always, msg)
			}
			if shouldCollectOp(parsedOp, context.driverOpsFiltered) {
				context.Collect(recordedOp, parsedOp, reply, msg)
			}
		}
		userInfoLogger.Logvf(Info, "(Connection %v) Connection ENDED.", connectionNum)
		context.ConnectionChansWaitGroup.Done()
	}()
	return ch
}

// Execute plays a particular command on an mgo socket.
func (context *ExecutionContext) Execute(op *RecordedOp, socket *mgo.MongoSocket) (Op, Replyable, error) {
	opToExec, err := op.RawOp.Parse()
	var reply Replyable

	if err != nil {
		return nil, nil, fmt.Errorf("ParseOpRawError: %v", err)
	}
	if opToExec == nil {
		toolDebugLogger.Logvf(Always, "Skipping incomplete op: %v", op.RawOp.Header.OpCode)
		return nil, nil, nil
	}
	switch replyable := opToExec.(type) {
	case *ReplyOp:
		context.AddFromFile(replyable, op)
	case *CommandReplyOp:
		context.AddFromFile(replyable, op)
	case *MsgOpReply:
		context.AddFromFile(replyable, op)
	default:
		if !context.driverOpsFiltered && IsDriverOp(opToExec) {
			return opToExec, nil, nil
		}
		if rewriteable, ok1 := opToExec.(cursorsRewriteable); ok1 {
			ok2, err := context.rewriteCursors(rewriteable, op.SeenConnectionNum)
			if err != nil {
				return opToExec, nil, err
			}
			if !ok2 {
				return opToExec, nil, nil
			}
		}
		// check if the op has a function to preprocess its data given the current
		// set of options
		if op, ok := opToExec.(Preprocessable); ok {
			op.Preprocess()
		}

		op.PlayedAt = &PreciseTime{time.Now()}

		reply, err = opToExec.Execute(socket)

		if err != nil {
			context.CursorIDMap.MarkFailed(op)
			return opToExec, reply, fmt.Errorf("error executing op: %v", err)
		}
		if reply != nil {
			context.AddFromWire(reply, op)
		}
	}
	context.handleCompletedReplies()
	return opToExec, reply, nil
}
