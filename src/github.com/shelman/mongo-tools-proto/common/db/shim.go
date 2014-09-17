package db

import (
	"errors"
	"fmt"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

const MaxBSONSize = 16 * 1024 * 1024

type ShimMode int

const (
	Dump ShimMode = iota
	Insert
)

type StorageShim struct {
	DBPath      string
	Database    string
	Collection  string
	Skip        int
	Limit       int
	ShimPath    string
	Query       string
	Mode        ShimMode
	shimProcess *exec.Cmd
}

//BSONSource wraps a stream
type BSONSource struct {
	Stream io.ReadCloser
	err    error
}

type DecodedBSONSource struct {
	reusableBuf []byte
	*BSONSource
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

func NewDecodedBSONSource(str *BSONSource) *DecodedBSONSource {
	return &DecodedBSONSource{make([]byte, MaxBSONSize), str}
}

func (decStrm *DecodedBSONSource) Close() error {
	return decStrm.Stream.Close()
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

	if shim.Mode == Dump {
		//Do nothing, since dump is the default behavior
	} else if shim.Mode == Insert {
		returnVal = append(returnVal, "--load")
	}
	return returnVal
}

type DocSource interface {
	Next(interface{}) bool
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
	bsonOut *BSONSink
}

func (bs *BSONSink) WriteBytes(buf []byte) (int, error) {
	return bs.writer.Write(buf)
}

func (ebs *EncodedBSONSink) WriteDoc(out interface{}) (int, error) {
	outbuf, err := bson.Marshal(out)
	if err != nil {
		return 0, fmt.Errorf("Failed to encode document into BSON: %v", err)
	}
	return ebs.bsonOut.WriteBytes(outbuf)
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

func checkExists(path string) (bool, error) {
	if _, err := os.Stat(path); err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}

var ShimNotFoundErr = errors.New("Shim not found")

func LocateShim() (string, error) {
	shimLoc := os.Getenv("MONGOSHIM")
	if shimLoc == "" {
		dir, err := filepath.Abs(filepath.Dir(os.Args[0]))
		if err != nil {
			return "", err
		}
		shimLoc = filepath.Join(dir, "mongoshim")
		if runtime.GOOS == "windows" {
			shimLoc += ".exe"
		}
	}

	if shimLoc == "" {
		return "", ShimNotFoundErr
	}

	exists, err := checkExists(shimLoc)
	if err != nil {
		return "", err
	}
	if !exists {
		return "", ShimNotFoundErr
	}
	return shimLoc, nil
}

//Open() starts the external shim process and returns an instance of BSONSource
//bound to its STDOUT stream.
func (shim *StorageShim) Open() (*BSONSource, *BSONSink, error) {
	args := buildArgs(*shim)
	cmd := exec.Command(shim.ShimPath, args...)
	stdOut, err := cmd.StdoutPipe()
	if err != nil {
		return nil, nil, err
	}
	stdErr, err := cmd.StderrPipe()
	if err != nil {
		return nil, nil, err
	}
	go func() {
		io.Copy(os.Stderr, stdErr)
	}()

	stdIn, err := cmd.StdinPipe()
	if err != nil {
		return nil, nil, err
	}

	err = cmd.Start()
	if err != nil {
		return nil, nil, err
	}

	return &BSONSource{stdOut, nil}, &BSONSink{stdIn}, nil
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
