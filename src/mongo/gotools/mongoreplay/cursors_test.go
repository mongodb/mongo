// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"sync"
	"testing"
	"time"
)

// TestFetchingCursorFromPreprocessManager tests that a CursorID set in the
// preprocessCursorManager can be properly retrieved. It makes a
// preprocessCursorManager and adds a fileCursorID that it expects to see during
// playback. It then calls SetCursor to simulate setting this CursorID from live
// traffic. Finally, it gets the cursor from the preprocessCursorManager to
// ensure the cursorID has been remapped correctly. It uses a select statement
// to establish a timeout incase the goroutine running GetCursor has not
// returned because the cursorID was not set properly.
func TestFetchingCursorFromPreprocessManager(t *testing.T) {
	fileCursor := int64(1234)
	wireCursor := int64(4567)
	// Make a cursorManager

	t.Log("Creating cursor manager")
	cursorManager := &preprocessCursorManager{
		cursorInfos: make(map[int64]*preprocessCursorInfo),
	}
	// Prepopulate it with cursor details
	cursorManager.cursorInfos[fileCursor] = &preprocessCursorInfo{
		successChan: make(chan struct{}),
		numUsesLeft: 1,
	}
	// Set its corresponding id from a 'wire'
	t.Log("Setting cursor from live traffic")
	cursorManager.SetCursor(fileCursor, wireCursor)

	var ok bool
	var cursor int64
	cursorFoundChan := make(chan struct{})

	// Fetch the id
	go func() {
		t.Log("Fetching cursor")
		cursor, ok = cursorManager.GetCursor(fileCursor, -1)
		cursorFoundChan <- struct{}{}
	}()

	//verify that it has been fetched properly
	select {
	case <-cursorFoundChan:
		t.Log("Cursor fetched")
	case <-time.After(time.Second * 30):
		t.Error("Timed out waiting for cursor to be set")
	}

	if !ok {
		t.Error("Can't find cursor in map")
	}
	if cursor != wireCursor {
		t.Errorf("Cursor ID incorrect. Expected:%v -- Found: %v", wireCursor, cursor)
	}
}

// TestPreprocessingFileWithOpCommand tests that a preprocessManager is
// correctly generated from from a known series of op_commands. It generates a
// commandreply and a command_op getmore which both use the same cursorID and
// feeds a channel with these ops in it to the newPreprocessCursorManager
// function. Finally, it verifies that the predefined cursorID was set in the
// manager.
func TestPreprocessingFileWithOpCommand(t *testing.T) {
	requestID := int32(1234)
	testCursorID := int64(4567)

	// Generate a channel with a getmore and a reply
	generator := newRecordedOpGenerator()
	var err error

	err = generator.generateCommandReply(requestID, testCursorID)
	if err != nil {
		t.Error(err)
	}

	err = generator.generateCommandGetMore(testCursorID, 0)
	if err != nil {
		t.Error(err)
	}
	close(generator.opChan)
	t.Log("Creating preprocessCursorManager with newPreprocessCursorManager")

	// Attempt to load these into the preprocessCursorManger
	preprocessManager, err := newPreprocessCursorManager((<-chan *RecordedOp)(generator.opChan))
	if err != nil {
		t.Error(err)
	}

	// Verify that the cursorID was entered into the map
	t.Log("Verifying that cursor was mapped")
	cursorInfo, ok := preprocessManager.cursorInfos[testCursorID]
	if !ok {
		t.Errorf("Cursor %v was supposed to be mapped, but wasn't", testCursorID)
		return
	}
	if cursorInfo.numUsesLeft != 1 {
		t.Errorf("Incorrect number of uses left for cursor %v. Should be: %d ---- Found: %d",
			testCursorID, 1, cursorInfo.numUsesLeft)
	}
}

// TestPreprocessingFile tests that a preprocessManager is correctly generated
// from from a known series of ops. It generates a reply and a getmore which
// both use the same cursorID and feeds a channel with these ops in it to the
// newPreprocessCursorManager function. Finally, it verifies that the predefined
// cursorID was set in the manager.
func TestPreprocessingFile(t *testing.T) {
	requestID := int32(1234)
	testCursorID := int64(4567)

	// Generate a channel with a getmore and a reply
	generator := newRecordedOpGenerator()
	var err error

	err = generator.generateReply(requestID, testCursorID)
	if err != nil {
		t.Error(err)
	}

	err = generator.generateGetMore(testCursorID, 0)
	if err != nil {
		t.Error(err)
	}
	close(generator.opChan)
	t.Log("Creating preprocessCursorManager with newPreprocessCursorManager")

	// Attempt to load these into the preprocessCursorManger
	preprocessManager, err := newPreprocessCursorManager((<-chan *RecordedOp)(generator.opChan))
	if err != nil {
		t.Error(err)
	}

	// Verify that the cursorID was entered into the map
	t.Log("Verifying that cursor was mapped")
	cursorInfo, ok := preprocessManager.cursorInfos[testCursorID]
	if !ok {
		t.Errorf("Cursor %v was supposed to be mapped, but wasn't", testCursorID)
	}
	if cursorInfo.numUsesLeft != 1 {
		t.Errorf("Incorrect number of uses left for cursor %v. Should be: %d ---- Found: %d",
			testCursorID, 1, cursorInfo.numUsesLeft)
	}
}

// TestBlockOnUnresolvedCursor verifies that a preprocessCursorManager will
// block execution on a call to GetCursor if the corresponding live cursorID has
// not been found to a cursorID that was mapped during preprocessing.
func TestBlockOnUnresolvedCursor(t *testing.T) {
	fileCursor := int64(1234)
	liveCursor := int64(4567)

	// Prepopulate a preprocessCursorManager with a details about a cursor
	cursorManager := &preprocessCursorManager{
		cursorInfos: make(map[int64]*preprocessCursorInfo),
		RWMutex:     sync.RWMutex{},
	}
	lock := &cursorManager.RWMutex
	cursorManager.cursorInfos[fileCursor] = &preprocessCursorInfo{
		successChan: make(chan struct{}),
		numUsesLeft: 1,
	}
	t.Log("Created preprocessCursorManager with prepopulated cursor")

	var retrievedCursor int64
	var ok bool

	go func() {
		if retrievedCursor != 0 {
			t.Error("Cursor map returned result before live cursor was mapped")
		}
		// Retrieve cursorInfo from map
		lock.RLock()
		cursorInfo, ok := cursorManager.cursorInfos[fileCursor]
		if !ok {
			t.Errorf("Cursor %v was supposed to be mapped, but wasn't", testCursorID)
		}
		lock.RUnlock()

		t.Log("Verifying that successChan not closed before cursor was set")
		// Verify that its successChan is not closed, which indicates that
		// GetCuror would block
		select {
		case <-cursorInfo.successChan:
			t.Error("successChan closed before mapping was complete")
		default:
		}

		// Set the corresponding cursorID from live traffic
		cursorManager.SetCursor(fileCursor, liveCursor)

		t.Log("Verifying that successChan closed after cursor was Set")
		// Verify that the successChan is closed and that execution of GetCursor
		// can continue
		select {
		case <-cursorInfo.successChan:
		case <-time.After(time.Second * 30):
			t.Error("successChan should have been closed after cursor mapping")
		}
	}()

	// Verify that fetched cursorID details are correct
	retrievedCursor, ok = cursorManager.GetCursor(fileCursor, -1)
	if !ok {
		t.Errorf("Cursor %v was supposed to be mapped, but wasn't", testCursorID)
	}
	if retrievedCursor != liveCursor {
		t.Errorf("Retrieved cursor was supposed to be mapped to %v but was %v instead", liveCursor, retrievedCursor)
	}
}

// TestSkipOutOfOrderCursor ensures that a reply containing a cursorID that is
// used by an operation scheduled to occur earlier on the same connection will
// be skipped. This 'out of order' may be caused by severe packet lose during
// traffic capture and would result in total deadlock of the program.
func TestSkipOutOfOrderCursor(t *testing.T) {
	requestID := int32(1234)
	testCursorID := int64(4567)
	generator := newRecordedOpGenerator()
	var err error

	// Generate ops out of order on the same connection to be mapped by the preprocessCursorManager
	err = generator.generateGetMore(testCursorID, 0)
	if err != nil {
		t.Error(err)
	}

	err = generator.generateReply(requestID, testCursorID)
	if err != nil {
		t.Error(err)
	}
	close(generator.opChan)

	t.Log("Generating preprocessCursorManager from channel with newPreprocessCursorManager")
	preprocessMap, err := newPreprocessCursorManager((generator.opChan))
	if err != nil {
		t.Error(err)
	}

	var remappedCursor int64
	var ok bool
	cursorFoundChan := make(chan struct{})

	// Attempt to get the live cursor
	go func() {
		t.Log("Fetching cursor")
		remappedCursor, ok = preprocessMap.GetCursor(testCursorID, 0)
		cursorFoundChan <- struct{}{}
	}()

	// Verify that it returns
	select {
	case <-cursorFoundChan:
	case <-time.After(time.Second * 30):
		t.Error("Timed out waiting for GetCursor to return")
	}
	// Verify that fetching the cursor returns false
	if ok {
		t.Errorf("Cursor %v was supposed to be skipped ", testCursorID)
	}
	if remappedCursor != 0 {
		t.Errorf("Incorrect cursor value for cursor: %v. Should be: %d ---- Found: %d",
			testCursorID, 0, remappedCursor)
	}
}

// TestSkipMarkFailed verifies that fetching a cursorID stops blocking if the op
// that was supposed to create the cursor it was waiting on fails to execute.
func TestSkipOnMarkFailed(t *testing.T) {
	requestID := int32(1234)
	testCursorID := int64(4567)
	generator := newRecordedOpGenerator()
	var err error

	// Generate the op that is going to fail, along with its reply and a getmore
	// that replies on its cursorID
	err = generator.generateQuery(struct{}{}, 0, requestID)
	if err != nil {
		t.Error(err)
	}
	err = generator.generateReply(requestID, testCursorID)
	if err != nil {
		t.Error(err)
	}
	err = generator.generateGetMore(testCursorID, 0)
	if err != nil {
		t.Error(err)
	}
	close(generator.opChan)
	preprocessChan := make(chan *RecordedOp, 10)
	var opToFail *RecordedOp

	go func() {
		for op := range generator.opChan {
			if op.RawOp.Header.OpCode == OpCodeQuery {
				opToFail = op
				op.SrcEndpoint = "a"
				op.DstEndpoint = "b"
			} else if op.RawOp.Header.OpCode == OpCodeReply {
				op.SrcEndpoint = "b"
				op.DstEndpoint = "a"
			}
			preprocessChan <- op
		}
		close(preprocessChan)
	}()

	t.Log("Creating preprocessCursorManager from generated ops")
	preprocessManager, err := newPreprocessCursorManager(preprocessChan)
	if err != nil {
		t.Error(err)
	}
	lock := &preprocessManager.RWMutex

	var retrievedCursor int64 = -1
	var ok bool

	go func() {
		if retrievedCursor != -1 {
			t.Error("Cursor map returned result before cursor was marked as failed")
		}
		lock.RLock()
		cursorInfo, ok := preprocessManager.cursorInfos[testCursorID]
		if !ok {
			t.Errorf("Cursor %v was supposed to be mapped, but wasn't", testCursorID)
		}
		lock.RUnlock()

		t.Log("Checking that successChan and failChan are still open before marking op as failed")
		select {
		case <-cursorInfo.failChan:
			t.Error("failChan closed before marked as failing")
		case <-cursorInfo.successChan:
			t.Error("successChan closed and should never have been")
		default:
		}

		t.Log("Marking op as failed")
		preprocessManager.MarkFailed(opToFail)

		// Verify that the failChan is closed, which allows GetCursor to continue
		select {
		case <-cursorInfo.failChan:
		case <-time.After(time.Second * 30):
			t.Error("failChan should have been closed after being marked as failed")
		}

	}()

	t.Log("Verify that GetCursor returns when op is marked as failed")
	retrievedCursor, ok = preprocessManager.GetCursor(testCursorID, -1)
	// Verify that fetching the associated cursor returns false
	if ok {
		t.Errorf("Cursor %v was supposed fail", testCursorID)
	}
}
