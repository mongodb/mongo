// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"testing"

	mgo "github.com/10gen/llmgo"
)

func TestCompleteReply(t *testing.T) {
	context := NewExecutionContext(&StatCollector{}, nil, &ExecutionOptions{})

	// AddFromWire takes a recorded request and a live reply to the re-execution
	// of that reply
	reply1 := &ReplyOp{}
	reply1.ReplyOp = mgo.ReplyOp{
		CursorId: 2500,
	}
	recordedOp1 := &RecordedOp{
		DstEndpoint: "a",
		SrcEndpoint: "b",
		RawOp: RawOp{
			Header: MsgHeader{
				RequestID: 1000,
			},
		},
		Generation: 0,
	}
	context.AddFromWire(reply1, recordedOp1)

	// AddFromFile takes a recorded reply and the contained reply
	reply2 := &ReplyOp{}
	reply2.ReplyOp = mgo.ReplyOp{
		CursorId: 1500,
	}

	recordedOp2 := &RecordedOp{
		DstEndpoint: "b",
		SrcEndpoint: "a",
		RawOp: RawOp{
			Header: MsgHeader{
				ResponseTo: 1000,
			},
		},
		Generation: 0,
	}
	context.AddFromFile(reply2, recordedOp2)
	if len(context.CompleteReplies) != 1 {
		t.Error("replies not completed")
	}
	context.handleCompletedReplies()

	cursorIDLookup, ok := context.CursorIDMap.GetCursor(1500, -1)
	if !ok {
		t.Error("can't find cursorID in map")
	}
	if cursorIDLookup != 2500 {
		t.Errorf("looked up cursorID is wrong: %v, should be 2500", cursorIDLookup)
	}
}
