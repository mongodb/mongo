// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"bytes"
	"io"
	"testing"
	"time"
)

func TestRepeatGeneration(t *testing.T) {
	recOp := &RecordedOp{
		Seen: &PreciseTime{time.Now()},
	}

	var buf bytes.Buffer
	wc := NopWriteCloser(&buf)
	file, err := playbackFileWriterFromWriteCloser(wc, "", PlaybackFileMetadata{})
	if err != nil {
		t.Fatalf("error creating playback file %v", err)
	}

	err = bsonToWriter(file, recOp)
	if err != nil {
		t.Fatalf("error writing to bson file %v", err)
	}

	rs := bytes.NewReader(buf.Bytes())
	playbackReader, err := playbackFileReaderFromReadSeeker(rs, "")
	if err != nil {
		t.Fatalf("unable to read from playback file %v", err)
	}

	repeat := 2
	opChan, errChan := playbackReader.OpChan(repeat)
	op1, ok := <-opChan
	if !ok {
		err, ok := <-errChan
		if ok {
			t.Logf("error: %v", err)
		}
		t.Fatalf("read of 0-generation op failed")
	}
	if op1.Generation != 0 {
		t.Errorf("generation of 0 generation op is %v", op1.Generation)
	}
	op2, ok := <-opChan
	if !ok {
		t.Fatalf("read of 1-generation op failed")
	}
	if op2.Generation != 1 {
		t.Errorf("generation of 1 generation op is %v", op2.Generation)
	}
	_, ok = <-opChan
	if ok {
		t.Errorf("Successfully read past end of op chan")
	}
	err = <-errChan
	if err != io.EOF {
		t.Errorf("should have eof at end, but got %v", err)
	}
}

func TestPlayOpEOF(t *testing.T) {
	ops := []RecordedOp{{
		Seen: &PreciseTime{time.Now()},
	}, {
		Seen: &PreciseTime{time.Now()},
		EOF:  true,
	}}
	var buf bytes.Buffer
	wc := NopWriteCloser(&buf)
	file, err := playbackFileWriterFromWriteCloser(wc, "", PlaybackFileMetadata{})
	if err != nil {
		t.Fatalf("error creating playback file %v", err)
	}

	for _, op := range ops {
		err := bsonToWriter(file, op)
		if err != nil {
			t.Fatalf("unable to write to playback file %v", err)
		}
	}

	rs := bytes.NewReader(buf.Bytes())
	playbackReader, err := playbackFileReaderFromReadSeeker(rs, "")
	if err != nil {
		t.Fatalf("unable to read from playback file %v", err)
	}

	repeat := 2
	opChan, errChan := playbackReader.OpChan(repeat)

	op1, ok := <-opChan
	if !ok {
		t.Fatalf("read of op1 failed")
	}
	if op1.EOF {
		t.Errorf("op1 should not be an EOF op")
	}
	op2, ok := <-opChan
	if !ok {
		t.Fatalf("read op2 failed")
	}
	if op2.EOF {
		t.Errorf("op2 should not be an EOF op")
	}
	op3, ok := <-opChan
	if !ok {
		t.Errorf("read of op3 failed")
	}
	if !op3.EOF {
		t.Errorf("op3 is not an EOF op")
	}

	_, ok = <-opChan
	if ok {
		t.Errorf("Successfully read past end of op chan")
	}
	err = <-errChan
	if err != io.EOF {
		t.Errorf("should have eof at end, but got %v", err)
	}
}
