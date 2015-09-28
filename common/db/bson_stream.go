package db

import (
	"fmt"
	"gopkg.in/mgo.v2/bson"
	"io"
)

// BSONSource reads documents from the underlying io.ReadCloser, Stream which
// wraps a stream of BSON documents.
type BSONSource struct {
	Stream io.ReadCloser
	err    error
}

// DecodedBSONSource reads documents from the underlying io.ReadCloser, Stream which
// wraps a stream of BSON documents.
type DecodedBSONSource struct {
	reusableBuf []byte
	RawDocSource
	err error
}

// RawDocSource wraps basic functions for reading a BSON source file.
type RawDocSource interface {
	LoadNextInto(into []byte) (bool, int32)
	Close() error
	Err() error
}

func NewBSONSource(in io.ReadCloser) *BSONSource {
	return &BSONSource{in, nil}
}

// Close closes the BSONSource, rendering it unusable for I/O.
// It returns an error, if any.
func (bs *BSONSource) Close() error {
	return bs.Stream.Close()
}

func NewDecodedBSONSource(ds RawDocSource) *DecodedBSONSource {
	return &DecodedBSONSource{make([]byte, MaxBSONSize), ds, nil}
}

// Err returns any error in the DecodedBSONSource or its RawDocSource.
func (dbs *DecodedBSONSource) Err() error {
	if dbs.err != nil {
		return dbs.err
	}
	return dbs.RawDocSource.Err()
}

// Next unmarshals the next BSON document into result. Returns true if no errors
// are encountered and false otherwise.
func (dbs *DecodedBSONSource) Next(result interface{}) bool {
	hasDoc, docSize := dbs.LoadNextInto(dbs.reusableBuf)
	if !hasDoc {
		return false
	}
	if err := bson.Unmarshal(dbs.reusableBuf[0:docSize], result); err != nil {
		dbs.err = err
		return false
	}
	dbs.err = nil
	return true
}

// LoadNextInto unmarshals the next BSON document into result. Returns a boolean
// indicating whether or not the operation was successful (true if no errors)
// and the size of the unmarshaled document.
func (bs *BSONSource) LoadNextInto(into []byte) (bool, int32) {
	// read the bson object size (a 4 byte integer)
	_, err := io.ReadAtLeast(bs.Stream, into[0:4], 4)
	if err != nil {
		if err != io.EOF {
			bs.err = err
			return false, 0
		}
		// we hit EOF right away, so we're at the end of the stream.
		bs.err = nil
		return false, 0
	}

	bsonSize := int32(
		(uint32(into[0]) << 0) |
			(uint32(into[1]) << 8) |
			(uint32(into[2]) << 16) |
			(uint32(into[3]) << 24),
	)

	// Verify that the size of the BSON object we are about to read can
	// actually fit into the buffer that was provided. If not, either the BSON is
	// invalid, or the buffer passed in is too small.
	// Verify that we do not have an invalid BSON document with size < 5.
	if bsonSize > int32(len(into)) || bsonSize < 5 {
		bs.err = fmt.Errorf("invalid BSONSize: %v bytes", bsonSize)
		return false, 0
	}
	_, err = io.ReadAtLeast(bs.Stream, into[4:int(bsonSize)], int(bsonSize-4))
	if err != nil {
		if err != io.EOF {
			bs.err = err
			return false, 0
		}
		// this case means we hit EOF but read a partial document,
		// so there's a broken doc in the stream. Treat this as error.
		bs.err = fmt.Errorf("invalid bson: %v", err)
		return false, 0
	}

	bs.err = nil
	return true, bsonSize
}

func (bs *BSONSource) Err() error {
	return bs.err
}
