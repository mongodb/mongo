package db

import (
	"fmt"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
)

//BSONSource wraps a stream
type BSONSource struct {
	Stream io.ReadCloser
	err    error
}

type DecodedBSONSource struct {
	reusableBuf []byte
	RawDocSource
	err error
}

func NewBSONSource(in io.ReadCloser) *BSONSource {
	return &BSONSource{in, nil}
}

func (bsonSource *BSONSource) Close() error {
	return bsonSource.Stream.Close()
}

type DocSink interface {
	WriteDoc(out interface{}) error
	Close() error
}

func NewDecodedBSONSource(str RawDocSource) *DecodedBSONSource {
	return &DecodedBSONSource{make([]byte, MaxBSONSize), str, nil}
}

func (decStrm *DecodedBSONSource) Err() error {
	if decStrm.err != nil {
		return decStrm.err
	}
	return decStrm.RawDocSource.Err()
}

func (decStrm *DecodedBSONSource) Next(into interface{}) bool {
	hasDoc, docSize := decStrm.LoadNextInto(decStrm.reusableBuf)
	if !hasDoc {
		return false
	}
	if err := bson.Unmarshal(decStrm.reusableBuf[0:docSize], into); err != nil {
		decStrm.err = err
		return false
	}
	decStrm.err = nil
	return true
}

type DocSource interface {
	Next(interface{}) bool
	Close() error
	Err() error
}

type RawDocSource interface {
	LoadNextInto(into []byte) (bool, int32)
	Close() error
	Err() error
}

type CursorDocSource struct {
	Iter    *mgo.Iter
	Session *mgo.Session
}

type BSONSink struct {
	writer io.WriteCloser
}

type EncodedBSONSink struct {
	BSONIn *BSONSink
}

func (bs *BSONSink) WriteBytes(buf []byte) (int, error) {
	return bs.writer.Write(buf)
}

func (ebs *EncodedBSONSink) WriteDoc(out interface{}) (int, error) {
	outbuf, err := bson.Marshal(out)
	if err != nil {
		return 0, fmt.Errorf("Failed to encode document into BSON: %v", err)
	}
	return ebs.BSONIn.WriteBytes(outbuf)
}

func (cds *CursorDocSource) Next(out interface{}) bool {
	return cds.Iter.Next(out)
}

func (cds *CursorDocSource) Close() error {
	defer cds.Session.Close()
	return cds.Iter.Close()
}

func (cds *CursorDocSource) Err() error {
	return cds.Iter.Err()
}

func (shim *BSONSource) LoadNextInto(into []byte) (bool, int32) {
	//Read the bson object size (a 4 byte integer)
	_, err := io.ReadAtLeast(shim.Stream, into[0:4], 4)
	if err != nil {
		if err != io.EOF {
			shim.err = err
			return false, 0
		} else {
			//We hit EOF right away, so we're at the end of the stream.
			shim.err = nil
			return false, 0
		}
	}

	bsonSize := int32(
		(uint32(into[0]) << 0) |
			(uint32(into[1]) << 8) |
			(uint32(into[2]) << 16) |
			(uint32(into[3]) << 24),
	)

	//Verify that the size of the BSON object we are about to read can
	//actually fit into the buffer that was provided. If not, either the BSON is
	//invalid, or the buffer passed in is too small.
	if bsonSize > int32(len(into)) {
		shim.err = fmt.Errorf("Invalid BSONSize: %v bytes", bsonSize)
		return false, 0
	}
	_, err = io.ReadAtLeast(shim.Stream, into[4:int(bsonSize)], int(bsonSize-4))
	if err != nil {
		if err != io.EOF {
			shim.err = err
			return false, 0
		} else {
			//This case means we hit EOF but read a partial document,
			//so there's a broken doc in the stream. Treat this as error.
			shim.err = fmt.Errorf("Invalid bson: %v", err)
			return false, 0
		}
	}

	shim.err = nil
	return true, bsonSize
}

func (shim *BSONSource) Err() error {
	return shim.err
}
