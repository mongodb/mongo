package db

import (
	"fmt"
	"gopkg.in/mgo.v2/bson"
	"io"
)

// BSONSource reads documents from the underlying io.ReadCloser, Stream which
// wraps a stream of BSON documents.
type BSONSource struct {
	reusableBuf []byte
	Stream      io.ReadCloser
	err         error
}

// DecodedBSONSource reads documents from the underlying io.ReadCloser, Stream which
// wraps a stream of BSON documents.
type DecodedBSONSource struct {
	RawDocSource
	err error
}

// RawDocSource wraps basic functions for reading a BSON source file.
type RawDocSource interface {
	LoadNext() []byte
	Close() error
	Err() error
}

// NewBSONSource creates a BSONSource with a reusable I/O buffer
func NewBSONSource(in io.ReadCloser) *BSONSource {
	return &BSONSource{make([]byte, MaxBSONSize), in, nil}
}

// NewBufferlessBSONSource creates a BSONSource without a reusable I/O buffer
func NewBufferlessBSONSource(in io.ReadCloser) *BSONSource {
	return &BSONSource{nil, in, nil}
}

// Close closes the BSONSource, rendering it unusable for I/O.
// It returns an error, if any.
func (bs *BSONSource) Close() error {
	return bs.Stream.Close()
}

func NewDecodedBSONSource(ds RawDocSource) *DecodedBSONSource {
	return &DecodedBSONSource{ds, nil}
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
	doc := dbs.LoadNext()
	if doc == nil {
		return false
	}
	if err := bson.Unmarshal(doc, result); err != nil {
		dbs.err = err
		return false
	}
	dbs.err = nil
	return true
}

// LoadNext reads and returns the next BSON document in the stream. If the
// BSONSource was created with NewBSONSource then each returned []byte will be
// a slice of a single reused I/O buffer. If the BSONSource was created with
// NewBufferlessBSONSource then each returend []byte will be individually
// allocated
func (bs *BSONSource) LoadNext() []byte {
	var into []byte
	if bs.reusableBuf == nil {
		into = make([]byte, 4)
	} else {
		into = bs.reusableBuf
	}
	// read the bson object size (a 4 byte integer)
	_, err := io.ReadAtLeast(bs.Stream, into[0:4], 4)
	if err != nil {
		if err != io.EOF {
			bs.err = err
			return nil
		}
		// we hit EOF right away, so we're at the end of the stream.
		bs.err = nil
		return nil
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
	if bsonSize > MaxBSONSize || bsonSize < 5 {
		bs.err = fmt.Errorf("invalid BSONSize: %v bytes", bsonSize)
		return nil
	}
	if int(bsonSize) > cap(into) {
		bigInto := make([]byte, bsonSize)
		copy(bigInto, into)
		into = bigInto
		if bs.reusableBuf != nil {
			bs.reusableBuf = bigInto
		}
	}
	into = into[:int(bsonSize)]
	_, err = io.ReadAtLeast(bs.Stream, into[4:], int(bsonSize-4))
	if err != nil {
		if err != io.EOF {
			bs.err = err
			return nil
		}
		// this case means we hit EOF but read a partial document,
		// so there's a broken doc in the stream. Treat this as error.
		bs.err = fmt.Errorf("invalid bson: %v", err)
		return nil
	}

	bs.err = nil
	return into
}

func (bs *BSONSource) Err() error {
	return bs.err
}
