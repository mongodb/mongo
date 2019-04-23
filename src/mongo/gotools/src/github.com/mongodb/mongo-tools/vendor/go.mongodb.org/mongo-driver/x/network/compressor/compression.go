// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package compressor // import "go.mongodb.org/mongo-driver/x/network/compressor"

import (
	"bytes"
	"compress/zlib"

	"io"

	"github.com/golang/snappy"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Compressor is the interface implemented by types that can compress and decompress wire messages. This is used
// when sending and receiving messages to and from the server.
type Compressor interface {
	CompressBytes(src, dest []byte) ([]byte, error)
	UncompressBytes(src, dest []byte) ([]byte, error)
	CompressorID() wiremessage.CompressorID
	Name() string
}

type writer struct {
	buf []byte
}

// Write appends bytes to the writer
func (w *writer) Write(p []byte) (n int, err error) {
	index := len(w.buf)
	if len(p) > cap(w.buf)-index {
		buf := make([]byte, 2*cap(w.buf)+len(p))
		copy(buf, w.buf)
		w.buf = buf
	}

	w.buf = w.buf[:index+len(p)]
	copy(w.buf[index:], p)
	return len(p), nil
}

// SnappyCompressor uses the snappy method to compress data
type SnappyCompressor struct {
}

// ZlibCompressor uses the zlib method to compress data
type ZlibCompressor struct {
	level      int
	zlibWriter *zlib.Writer
}

// CompressBytes uses snappy to compress a slice of bytes.
func (s *SnappyCompressor) CompressBytes(src, dest []byte) ([]byte, error) {
	dest = dest[:0]
	dest = snappy.Encode(dest, src)
	return dest, nil
}

// UncompressBytes uses snappy to uncompress a slice of bytes.
func (s *SnappyCompressor) UncompressBytes(src, dest []byte) ([]byte, error) {
	var err error
	dest, err = snappy.Decode(dest, src)
	if err != nil {
		return dest, err
	}

	return dest, nil
}

// CompressorID returns the ID for the snappy compressor.
func (s *SnappyCompressor) CompressorID() wiremessage.CompressorID {
	return wiremessage.CompressorSnappy
}

// Name returns the string name for the snappy compressor.
func (s *SnappyCompressor) Name() string {
	return "snappy"
}

// CompressBytes uses zlib to compress a slice of bytes.
func (z *ZlibCompressor) CompressBytes(src, dest []byte) ([]byte, error) {
	output := &writer{
		buf: dest[:0],
	}

	z.zlibWriter.Reset(output)

	_, err := z.zlibWriter.Write(src)
	if err != nil {
		_ = z.zlibWriter.Close()
		return output.buf, err
	}

	err = z.zlibWriter.Close()
	if err != nil {
		return output.buf, err
	}
	return output.buf, nil
}

// UncompressBytes uses zlib to uncompress a slice of bytes. It assumes dest is empty and is the exact size that it
// needs to be.
func (z *ZlibCompressor) UncompressBytes(src, dest []byte) ([]byte, error) {
	reader := bytes.NewReader(src)
	zlibReader, err := zlib.NewReader(reader)

	if err != nil {
		return dest, err
	}
	defer func() {
		_ = zlibReader.Close()
	}()

	_, err = io.ReadFull(zlibReader, dest)
	if err != nil {
		return dest, err
	}

	return dest, nil
}

// CompressorID returns the ID for the zlib compressor.
func (z *ZlibCompressor) CompressorID() wiremessage.CompressorID {
	return wiremessage.CompressorZLib
}

// Name returns the name for the zlib compressor.
func (z *ZlibCompressor) Name() string {
	return "zlib"
}

// CreateSnappy creates a snappy compressor
func CreateSnappy() Compressor {
	return &SnappyCompressor{}
}

// CreateZlib creates a zlib compressor
func CreateZlib(level *int) (Compressor, error) {
	var l int

	if level == nil {
		l = wiremessage.DefaultZlibLevel
	} else {
		l = *level
	}

	if l < zlib.NoCompression {
		l = wiremessage.DefaultZlibLevel
	}

	if l > zlib.BestCompression {
		l = zlib.BestCompression
	}

	var compressBuf bytes.Buffer
	zlibWriter, err := zlib.NewWriterLevel(&compressBuf, l)

	if err != nil {
		return &ZlibCompressor{}, err
	}

	return &ZlibCompressor{
		level:      l,
		zlibWriter: zlibWriter,
	}, nil
}
