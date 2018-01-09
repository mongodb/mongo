// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/10gen/llmgo/bson"
	"github.com/mongodb/mongo-tools/common/util"
)

const PlaybackFileVersion = 1

type PlaybackFileMetadata struct {
	PlaybackFileVersion int
	DriverOpsFiltered   bool
}

// PlaybackFileReader stores the necessary information for a playback source,
// which is just an io.ReadCloser.
type PlaybackFileReader struct {
	io.ReadSeeker
	fname string

	metadata PlaybackFileMetadata
}

// PlaybackFileWriter stores the necessary information for a playback destination,
// which is an io.WriteCloser and its location.
type PlaybackFileWriter struct {
	io.WriteCloser
	fname string

	metadata PlaybackFileMetadata
}

// GzipReadSeeker wraps an io.ReadSeeker for gzip reading
type GzipReadSeeker struct {
	readSeeker io.ReadSeeker
	*gzip.Reader
}

// NewPlaybackFileReader initializes a new PlaybackFileReader
func NewPlaybackFileReader(filename string, gzip bool) (*PlaybackFileReader, error) {
	var readSeeker io.ReadSeeker

	readSeeker, err := os.Open(filename)
	if err != nil {
		return nil, err
	}

	if gzip {
		readSeeker, err = NewGzipReadSeeker(readSeeker)
		if err != nil {
			return nil, err
		}
	}

	return playbackFileReaderFromReadSeeker(readSeeker, filename)
}

func playbackFileReaderFromReadSeeker(rs io.ReadSeeker, filename string) (*PlaybackFileReader, error) {

	// read the metadata from the file
	metadata := new(PlaybackFileMetadata)
	err := bsonFromReader(rs, metadata)
	if err != nil {
		return nil, fmt.Errorf("error reading metadata: %v", err)
	}

	return &PlaybackFileReader{
		ReadSeeker: rs,
		fname:      filename,

		metadata: *metadata,
	}, nil
}

// NextRecordedOp iterates through the PlaybackFileReader to yield the next
// RecordedOp. It returns io.EOF when successfully complete.
func (file *PlaybackFileReader) NextRecordedOp() (*RecordedOp, error) {
	doc := new(RecordedOp)
	err := bsonFromReader(file, doc)
	if err != nil {
		if err != io.EOF {
			err = fmt.Errorf("ReadDocument Error: %v", err)
		}
		return nil, err
	}
	return doc, nil
}

// NewPlaybackFileWriter initializes a new PlaybackFileWriter
func NewPlaybackFileWriter(playbackFileName string, driverOpsFiltered, isGzipWriter bool) (*PlaybackFileWriter, error) {
	metadata := PlaybackFileMetadata{
		PlaybackFileVersion: PlaybackFileVersion,
		DriverOpsFiltered:   driverOpsFiltered,
	}

	toolDebugLogger.Logvf(DebugLow, "Opening playback file %v", playbackFileName)
	file, err := os.Create(playbackFileName)
	if err != nil {
		return nil, fmt.Errorf("error opening playback file to write to: %v", err)
	}

	var wc io.WriteCloser
	wc = file

	if isGzipWriter {
		wc = &util.WrappedWriteCloser{gzip.NewWriter(file), file}
	}

	return playbackFileWriterFromWriteCloser(wc, playbackFileName, metadata)
}

func playbackFileWriterFromWriteCloser(wc io.WriteCloser, filename string,
	metadata PlaybackFileMetadata) (*PlaybackFileWriter, error) {

	bsonBytes, err := bson.Marshal(metadata)
	if err != nil {
		return nil, fmt.Errorf("error writing metadata: %v", err)
	}

	_, err = wc.Write(bsonBytes)
	if err != nil {
		return nil, fmt.Errorf("error writing metadata: %v", err)
	}

	return &PlaybackFileWriter{
		WriteCloser: wc,
		fname:       filename,

		metadata: metadata,
	}, nil

}

// NewGzipReadSeeker initializes a new GzipReadSeeker
func NewGzipReadSeeker(rs io.ReadSeeker) (*GzipReadSeeker, error) {
	gzipReader, err := gzip.NewReader(rs)
	if err != nil {
		return nil, err
	}
	return &GzipReadSeeker{rs, gzipReader}, nil
}

// Seek sets the offset for the next Read, and can only seek to the
// beginning of the file.
func (g *GzipReadSeeker) Seek(offset int64, whence int) (int64, error) {
	if whence != 0 || offset != 0 {
		return 0, fmt.Errorf("GzipReadSeeker can only seek to beginning of file")
	}
	_, err := g.readSeeker.Seek(offset, whence)
	if err != nil {
		return 0, err
	}
	g.Reset(g.readSeeker)
	return 0, nil
}

// OpChan runs a goroutine that will read and unmarshal recorded ops
// from a file and push them in to a recorded op chan. Any errors encountered
// are pushed to an error chan. Both the recorded op chan and the error chan are
// returned by the function.
// The error chan won't be readable until the recorded op chan gets closed.
func (pfReader *PlaybackFileReader) OpChan(repeat int) (<-chan *RecordedOp, <-chan error) {
	ch := make(chan *RecordedOp)
	e := make(chan error)

	var last time.Time
	var first time.Time
	var loopDelta time.Duration
	go func() {
		defer close(e)
		e <- func() error {
			defer close(ch)
			toolDebugLogger.Logv(Info, "Beginning playback file read")
			for generation := 0; generation < repeat; generation++ {
				_, err := pfReader.Seek(0, 0)
				if err != nil {
					return fmt.Errorf("PlaybackFile Seek: %v", err)
				}

				// Must read the metadata since file was seeked to 0
				metadata := new(PlaybackFileMetadata)
				err = bsonFromReader(pfReader, metadata)
				if err != nil {
					return fmt.Errorf("bson read error: %v", err)
				}

				var order int64
				for {
					recordedOp, err := pfReader.NextRecordedOp()
					if err != nil {
						if err == io.EOF {
							break
						}
						return err
					}
					last = recordedOp.Seen.Time
					if first.IsZero() {
						first = recordedOp.Seen.Time
					}
					recordedOp.Seen.Time = recordedOp.Seen.Add(loopDelta)
					recordedOp.Generation = generation
					recordedOp.Order = order
					// We want to suppress EOF's unless you're in the last
					// generation because all of the ops for one connection
					// across different generations get executed in the same
					// session. We don't want to close the session until the
					// connection closes in the last generation.
					if !recordedOp.EOF || generation == repeat-1 {
						ch <- recordedOp
					}
					order++
				}
				toolDebugLogger.Logvf(DebugHigh, "generation: %v", generation)
				loopDelta += last.Sub(first)
				first = time.Time{}
				continue
			}
			return io.EOF
		}()
	}()
	return ch, e
}
