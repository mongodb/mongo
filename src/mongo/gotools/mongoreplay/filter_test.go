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

	"github.com/10gen/llmgo/bson"
)

func TestRemoveDriverOpsFromFile(t *testing.T) {
	cases := []struct {
		name string

		driverOpsToGenerate   []string
		numInsertsToGenerate  int
		shouldRemoveDriverOps bool

		numOpsExpectedAfterFilter int
	}{
		{
			"filter driver ops",

			[]string{"isMaster", "ping", "getnonce"},
			4,
			true,

			4,
		},
		{
			"no driver ops in file",

			[]string{},
			4,
			true,

			4,
		},
		{
			"don't filter driver ops",

			[]string{"isMaster", "ping", "getnonce"},
			4,
			false,

			7,
		},
	}
	for _, c := range cases {
		t.Logf("running case: %s\n", c.name)
		// make an iowriter that just buffers
		b := &bytes.Buffer{}
		bufferFile := NopWriteCloser(b)

		playbackWriter, err := playbackFileWriterFromWriteCloser(bufferFile, "file", PlaybackFileMetadata{})
		if err != nil {
			t.Fatalf("couldn't create playbackfile writer %v", err)
		}

		// start a goroutine to write recorded ops to the opChan
		generator := newRecordedOpGenerator()
		go func() {
			defer close(generator.opChan)
			t.Logf("Generating %d inserts\n", c.numInsertsToGenerate)
			err := generator.generateInsertHelper("insert", 0, c.numInsertsToGenerate)
			if err != nil {
				t.Error(err)
			}
			t.Log("Generating driver ops")
			for _, opName := range c.driverOpsToGenerate {
				err = generator.generateCommandOp(opName, bson.D{}, 123)
				if err != nil {
					t.Error(err)
				}
			}
		}()

		// run Filter to remove the driver op from the file
		if err := Filter(generator.opChan, []*PlaybackFileWriter{playbackWriter}, c.shouldRemoveDriverOps, time.Time{}); err != nil {
			t.Error(err)
		}

		rs := bytes.NewReader(b.Bytes())
		// open a reader into the written output
		playbackReader, err := playbackFileReaderFromReadSeeker(rs, "")
		if err != nil {
			t.Fatalf("couldn't create playbackfile reader %v", err)
		}
		opChan, errChan := playbackReader.OpChan(1)

		// loop over the found operations and verify that the correct number and
		// types of operations are found
		numOpsFound := 0
		numDriverOpsFound := 0
		for op := range opChan {
			numOpsFound++
			parsedOp, err := op.RawOp.Parse()
			if err != nil {
				t.Error(err)
			}

			if IsDriverOp(parsedOp) {
				numDriverOpsFound++
			}
		}

		if c.shouldRemoveDriverOps && numDriverOpsFound > 0 {
			t.Errorf("expected to have removed driver ops but instead found %d", numDriverOpsFound)
		}

		if c.numOpsExpectedAfterFilter != numOpsFound {
			t.Errorf("expected to have found %d total ops after filter but instead found %d", c.numOpsExpectedAfterFilter, numOpsFound)
		}
		err = <-errChan
		if err != io.EOF {
			t.Errorf("should have eof at end, but got %v", err)
		}
	}
}

func TestSplitInputFile(t *testing.T) {
	cases := []struct {
		name string

		numPlaybackFiles    int
		numConnections      int
		numOpsPerConnection int
	}{
		{
			"one file",
			1,
			10,
			10,
		},
		{
			"multi file",
			5,
			10,
			2,
		},
	}
	for _, c := range cases {
		t.Logf("running case: %s\n", c.name)
		outfiles := make([]*PlaybackFileWriter, c.numPlaybackFiles)
		buffers := make([]*bytes.Buffer, c.numPlaybackFiles)

		// create a buffer to represent each specified playback file to write
		for i := 0; i < c.numPlaybackFiles; i++ {
			b := &bytes.Buffer{}
			buffers[i] = b

			bufferFile := NopWriteCloser(b)
			playbackWriter, err := playbackFileWriterFromWriteCloser(bufferFile, "testfile", PlaybackFileMetadata{})
			if err != nil {
				t.Fatalf("couldn't create playbackfile writer %v", err)
			}
			outfiles[i] = playbackWriter
		}

		// make an channel to push all recorded connections into
		opChan := make(chan *RecordedOp)
		go func() {
			t.Logf("generating %d recorded connections\n", c.numConnections)
			for i := 0; i < c.numConnections; i++ {
				generator := newRecordedOpGenerator()
				generator.generateInsertHelper("insert", 0, c.numOpsPerConnection)
				close(generator.opChan)
				for recordedOp := range generator.opChan {
					recordedOp.SeenConnectionNum = int64(i)
					opChan <- recordedOp
				}
			}
			close(opChan)
		}()

		// run the main filter routine with the given input
		if err := Filter(opChan, outfiles, false, time.Time{}); err != nil {
			t.Error(err)
		}

		// ensure that each file contains only ops from the connection determined by
		// connectionNum % numFiles == filenum
		t.Log("verifying connections correctly split")
		for fileNum, writtenBuffer := range buffers {
			rs := bytes.NewReader(writtenBuffer.Bytes())
			playbackReader, err := playbackFileReaderFromReadSeeker(rs, "")
			if err != nil {
				t.Fatalf("couldn't create playbackfile reader %v", err)
			}
			opChan, errChan := playbackReader.OpChan(1)

			for op := range opChan {
				expectedFileNum := op.SeenConnectionNum % int64(len(outfiles))
				if expectedFileNum != int64(fileNum) {
					t.Errorf("expected op with connection number %d to be in file"+
						"%d, but instead it was found in file %d", op.SeenConnectionNum, expectedFileNum, fileNum)
				}
			}
			err = <-errChan
			if err != io.EOF {
				t.Errorf("should have eof at end, but got %v", err)
			}
		}
	}
}

func TestRemoveOpsBeforeTime(t *testing.T) {
	// array of times to use for testing
	timesForTest := make([]time.Time, 16)
	now := time.Now()
	for i := range timesForTest {
		timesForTest[i] = now.Add(time.Second * time.Duration(i))
	}

	cases := []struct {
		name string

		timeToTruncateBefore time.Time
		timesOfRecordedOps   []time.Time

		numOpsExpectedAfterFilter int
	}{
		{
			"no truncation",

			time.Time{},
			timesForTest,
			16,
		},
		{
			"truncate all but one",

			timesForTest[len(timesForTest)-1],
			timesForTest,
			1,
		},
		{
			"truncate half",

			timesForTest[(len(timesForTest))/2],
			timesForTest,

			8,
		},
	}
	for _, c := range cases {
		t.Logf("running case: %s\n", c.name)

		// create a bytes buffer to write output into
		b := &bytes.Buffer{}
		bufferFile := NopWriteCloser(b)

		playbackWriter, err := playbackFileWriterFromWriteCloser(bufferFile, "file", PlaybackFileMetadata{})
		if err != nil {
			t.Fatalf("couldn't create playbackfile writer %v", err)
		}

		//create a recorded op for each time specified
		inputOpChan := make(chan *RecordedOp)
		go func() {
			generator := newRecordedOpGenerator()
			generator.generateInsertHelper("insert", 0, len(c.timesOfRecordedOps))
			close(generator.opChan)
			i := 0
			for recordedOp := range generator.opChan {
				recordedOp.Seen = &PreciseTime{c.timesOfRecordedOps[i]}
				inputOpChan <- recordedOp
				i++
			}
			close(inputOpChan)
		}()

		// run the main filter routine with the given input
		if err := Filter(inputOpChan, []*PlaybackFileWriter{playbackWriter}, false, c.timeToTruncateBefore); err != nil {
			t.Error(err)
		}

		rs := bytes.NewReader(b.Bytes())
		playbackReader, err := playbackFileReaderFromReadSeeker(rs, "")
		if err != nil {
			t.Fatalf("couldn't create playbackfile reader %v", err)
		}
		resultOpChan, errChan := playbackReader.OpChan(1)

		numOpsSeen := 0
		for op := range resultOpChan {
			numOpsSeen++
			if op.Seen.Time.Before(c.timeToTruncateBefore) {
				t.Errorf("execpected op with time %v to be truncated", op.Seen.Time)
			}
		}

		if numOpsSeen != c.numOpsExpectedAfterFilter {
			t.Errorf("expected to see %d ops but instead saw %d", c.numOpsExpectedAfterFilter, numOpsSeen)
		}

		err = <-errChan
		if err != io.EOF {
			t.Errorf("should have eof at end, but got %v", err)
		}
	}
}

// convienence function for adding a close method to an io.Writer
func NopWriteCloser(w io.Writer) io.WriteCloser {
	return &nopWriteCloser{w}
}

type nopWriteCloser struct {
	io.Writer
}

func (wc *nopWriteCloser) Close() error {
	return nil
}
