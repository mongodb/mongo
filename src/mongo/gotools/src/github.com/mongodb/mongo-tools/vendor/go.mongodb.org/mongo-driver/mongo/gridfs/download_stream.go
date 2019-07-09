// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package gridfs

import (
	"context"
	"errors"
	"io"
	"math"
	"time"

	"go.mongodb.org/mongo-driver/mongo"
)

// ErrWrongIndex is used when the chunk retrieved from the server does not have the expected index.
var ErrWrongIndex = errors.New("chunk index does not match expected index")

// ErrWrongSize is used when the chunk retrieved from the server does not have the expected size.
var ErrWrongSize = errors.New("chunk size does not match expected size")

var errNoMoreChunks = errors.New("no more chunks remaining")

// DownloadStream is a io.Reader that can be used to download a file from a GridFS bucket.
type DownloadStream struct {
	numChunks     int32
	chunkSize     int32
	cursor        *mongo.Cursor
	done          bool
	closed        bool
	buffer        []byte // store up to 1 chunk if the user provided buffer isn't big enough
	bufferStart   int
	bufferEnd     int
	expectedChunk int32 // index of next expected chunk
	readDeadline  time.Time
	fileLen       int64
}

func newDownloadStream(cursor *mongo.Cursor, chunkSize int32, fileLen int64) *DownloadStream {
	numChunks := int32(math.Ceil(float64(fileLen) / float64(chunkSize)))

	return &DownloadStream{
		numChunks: numChunks,
		chunkSize: chunkSize,
		cursor:    cursor,
		buffer:    make([]byte, chunkSize),
		done:      cursor == nil,
		fileLen:   fileLen,
	}
}

// Close closes this download stream.
func (ds *DownloadStream) Close() error {
	if ds.closed {
		return ErrStreamClosed
	}

	ds.closed = true
	return nil
}

// SetReadDeadline sets the read deadline for this download stream.
func (ds *DownloadStream) SetReadDeadline(t time.Time) error {
	if ds.closed {
		return ErrStreamClosed
	}

	ds.readDeadline = t
	return nil
}

// Read reads the file from the server and writes it to a destination byte slice.
func (ds *DownloadStream) Read(p []byte) (int, error) {
	if ds.closed {
		return 0, ErrStreamClosed
	}

	if ds.done {
		return 0, io.EOF
	}

	ctx, cancel := deadlineContext(ds.readDeadline)
	if cancel != nil {
		defer cancel()
	}

	bytesCopied := 0
	var err error
	for bytesCopied < len(p) {
		if ds.bufferStart >= ds.bufferEnd {
			// Buffer is empty and can load in data from new chunk.
			err = ds.fillBuffer(ctx)
			if err != nil {
				if err == errNoMoreChunks {
					if bytesCopied == 0 {
						ds.done = true
						return 0, io.EOF
					}
					return bytesCopied, nil
				}
				return bytesCopied, err
			}
		}

		copied := copy(p[bytesCopied:], ds.buffer[ds.bufferStart:ds.bufferEnd])

		bytesCopied += copied
		ds.bufferStart += copied
	}

	return len(p), nil
}

// Skip skips a given number of bytes in the file.
func (ds *DownloadStream) Skip(skip int64) (int64, error) {
	if ds.closed {
		return 0, ErrStreamClosed
	}

	if ds.done {
		return 0, nil
	}

	ctx, cancel := deadlineContext(ds.readDeadline)
	if cancel != nil {
		defer cancel()
	}

	var skipped int64
	var err error

	for skipped < skip {
		if ds.bufferStart == 0 {
			err = ds.fillBuffer(ctx)
			if err != nil {
				if err == errNoMoreChunks {
					return skipped, nil
				}

				return skipped, err
			}
		}

		// try to skip whole chunk if possible
		toSkip := 0
		if skip-skipped < int64(len(ds.buffer)) {
			// can skip whole chunk
			toSkip = len(ds.buffer)
		} else {
			// can only skip part of buffer
			toSkip = int(skip - skipped)
		}

		skipped += int64(toSkip)
		ds.bufferStart = (ds.bufferStart + toSkip) % (int(ds.chunkSize))
	}

	return skip, nil
}

func (ds *DownloadStream) fillBuffer(ctx context.Context) error {
	if !ds.cursor.Next(ctx) {
		ds.done = true
		return errNoMoreChunks
	}

	chunkIndex, err := ds.cursor.Current.LookupErr("n")
	if err != nil {
		return err
	}

	if chunkIndex.Int32() != ds.expectedChunk {
		return ErrWrongIndex
	}

	ds.expectedChunk++
	data, err := ds.cursor.Current.LookupErr("data")
	if err != nil {
		return err
	}

	_, dataBytes := data.Binary()
	copied := copy(ds.buffer, dataBytes)

	bytesLen := int32(len(dataBytes))
	if ds.expectedChunk == ds.numChunks {
		// final chunk can be fewer than ds.chunkSize bytes
		bytesDownloaded := int64(ds.chunkSize) * (int64(ds.expectedChunk) - int64(1))
		bytesRemaining := ds.fileLen - int64(bytesDownloaded)

		if int64(bytesLen) != bytesRemaining {
			return ErrWrongSize
		}
	} else if bytesLen != ds.chunkSize {
		// all intermediate chunks must have size ds.chunkSize
		return ErrWrongSize
	}

	ds.bufferStart = 0
	ds.bufferEnd = copied

	return nil
}
