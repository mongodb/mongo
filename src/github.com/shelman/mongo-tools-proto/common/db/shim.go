package db

import (
	"fmt"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"os/exec"
)

const MaxBSONSize = 16 * 1024 * 1024

type StorageShim struct {
	DBPath     string
	Database   string
	Collection string
	Skip       int
	Limit      int
	ShimPath   string
	Query      string
	process    *exec.Cmd
}

//BSONStream wraps a stream
type BSONStream struct {
	//a reusable buffer for holding the raw bytes of a BSON document
	Stream io.ReadCloser
	err    error
}

type DecodedBSONStream struct {
	reusableBuf []byte
	*BSONStream
}

func NewDecodedBSONStream(str *BSONStream) *DecodedBSONStream {
	return &DecodedBSONStream{make([]byte, MaxBSONSize), str}
}

func (decStrm *DecodedBSONStream) Close() error {
	return nil
	//noop for now
	//TODO make this do cleanup
}

func (decStrm *DecodedBSONStream) Next(into interface{}) bool {
	hasDoc, docSize := decStrm.LoadNextInto(decStrm.reusableBuf)
	if !hasDoc {
		return false
	}
	err := bson.Unmarshal(decStrm.reusableBuf[0:docSize], into)
	if err != nil {
		decStrm.err = err
		return false
	}
	decStrm.err = nil
	return true

}

func buildArgs(shim StorageShim) []string {
	returnVal := []string{}
	if shim.DBPath != "" {
		returnVal = append(returnVal, "--dbpath", shim.DBPath)
	}
	if shim.Database != "" {
		returnVal = append(returnVal, "-d", shim.Database)
	}
	if shim.Collection != "" {
		returnVal = append(returnVal, "-c", shim.Collection)
	}
	if shim.Query != "" {
		returnVal = append(returnVal, "--query", shim.Query)
	}
	return returnVal
}

//Open() starts the external shim process and returns an instance of BSONStream
//bound to its STDOUT stream.
func (shim *StorageShim) Open() (*BSONStream, error) {
	args := buildArgs(*shim)
	cmd := exec.Command(shim.ShimPath, args...)
	stdOut, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	stdErr, err := cmd.StderrPipe()
	if err != nil {
		return nil, err
	}
	go func() {
		io.Copy(os.Stderr, stdErr)
	}()

	err = cmd.Start()
	if err != nil {
		return nil, err
	}
	return &BSONStream{stdOut, nil}, nil
}

//readBytes is a helper that reads 'length' bytes from the Reader, making
//multiple calls to Read() if necessary.
//Returns an error if the underlying reader hits an error on Read(), or if
//the reader encounters EOF. The caller should check that bytesRead == length
//in cases where the returned error is EOF.
func readBytes(r io.Reader, into []byte, offset, length int) (int, error) {
	if length > len(into) {
		panic("Length for read is greater than buffer size")
	}
	bytesRead := 0
	for bytesRead < length {
		n, err := r.Read(into[offset+bytesRead : offset+length])
		bytesRead += n
		if err != nil {
			return bytesRead, err
		}
	}
	//The correct number of bytes has been read and we haven't hit EOF
	return bytesRead, nil
}

func (shim *BSONStream) LoadNextInto(into []byte) (bool, int32) {
	//Read the bson object size (a 4 byte integer)
	n, err := readBytes(shim.Stream, into, 0, 4)
	if err != nil {
		if err != io.EOF {
			shim.err = err
		} else {
			//Hit end of stream , no more docs to iterate.
			if n == 0 {
				shim.err = nil
				return false, 0
			} else {
				//This case means we hit EOF but read a partial document,
				//so there's a broken doc in the stream. Treat this as error.
				shim.err = fmt.Errorf("Invalid bson")
				return false, 0
			}
		}
		return false, 0
	}

	bsonSize := int32((uint32(into[0]) << 0) |
		(uint32(into[1]) << 8) |
		(uint32(into[2]) << 16) |
		uint32(into[3])<<24)

	//Verify that the size of the BSON object we are about to read can
	//actually fit into the buffer that was provided. If not, either the BSON is
	//invalid, or the buffer passed in is too small.
	if bsonSize > int32(len(into)) {
		shim.err = fmt.Errorf("Invalid BSONSize: %v bytes", bsonSize)
		return false, 0
	}
	n, err = readBytes(shim.Stream, into, 4, int(bsonSize-4))
	if err != nil {
		if err == io.EOF {
			//Hit end of stream , no more docs to iterate.
			if n == 0 {
				shim.err = nil
				return false, 0
			} else {
				//This case means we hit EOF but read a partial document,
				//so there's a broken doc in the stream. Treat this as error.
				shim.err = fmt.Errorf("Invalid bson")
				return false, 0
			}
		} else {
			shim.err = err
			return false, 0
		}
	}
	shim.err = nil
	return true, bsonSize
}

func (shim *BSONStream) Err() error {
	return shim.err
}

/*

func (shim *BSONStream) Next(into []byte) {
	shim.
}
*/
