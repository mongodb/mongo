package archive

import (
	"bytes"
	"hash"
	"hash/crc32"
	"io"
	"os"
	"testing"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

var testIntents = []*intents.Intent{
	&intents.Intent{
		DB:       "foo",
		C:        "bar",
		Location: "foo.bar",
	},
	&intents.Intent{
		DB:       "ding",
		C:        "bats",
		Location: "ding.bats",
	},
	&intents.Intent{
		DB:       "flim",
		C:        "flam.fooey",
		Location: "flim.flam.fooey",
	},
	&intents.Intent{
		DB:       "crow",
		C:        "bar",
		Location: "crow.bar",
	},
}

type testDoc struct {
	Bar int
	Baz string
}

type closingBuffer struct {
	bytes.Buffer
}

func (*closingBuffer) Close() error {
	return nil
}

type testNotifier struct{}

func (n *testNotifier) Notify() {}

func TestBasicMux(t *testing.T) {
	var err error

	Convey("with 10000 docs in each of five collections", t, func() {
		buf := &closingBuffer{bytes.Buffer{}}

		mux := NewMultiplexer(buf, new(testNotifier))
		muxIns := map[string]*MuxIn{}

		inChecksum := map[string]hash.Hash{}
		inLengths := map[string]*int{}
		outChecksum := map[string]hash.Hash{}
		outLengths := map[string]*int{}

		// To confirm that what we multiplex is the same as what we demultiplex, we
		// create input and output hashes for each namespace. After we finish
		// multiplexing and demultiplexing we will compare all of the CRCs for each
		// namespace
		errChan := make(chan error)
		makeIns(testIntents, mux, inChecksum, muxIns, inLengths, errChan)

		Convey("each document should be multiplexed", func() {
			go mux.Run()

			for range testIntents {
				err := <-errChan
				So(err, ShouldBeNil)
			}
			close(mux.Control)
			err = <-mux.Completed
			So(err, ShouldBeNil)

			demux := &Demultiplexer{In: buf}
			demuxOuts := map[string]*RegularCollectionReceiver{}

			errChan := make(chan error)
			makeOuts(testIntents, demux, outChecksum, demuxOuts, outLengths, errChan)

			Convey("and demultiplexed successfully", func() {
				demux.Run()
				So(err, ShouldBeNil)

				for range testIntents {
					err := <-errChan
					So(err, ShouldBeNil)
				}
				for _, dbc := range testIntents {
					ns := dbc.Namespace()
					So(*inLengths[ns], ShouldEqual, *outLengths[ns])
					inSum := inChecksum[ns].Sum([]byte{})
					outSum := outChecksum[ns].Sum([]byte{})
					So(inSum, ShouldResemble, outSum)
				}
			})
		})
	})
	return
}

func TestParallelMux(t *testing.T) {
	Convey("parallel mux/demux over a pipe", t, func() {
		readPipe, writePipe, err := os.Pipe()
		So(err, ShouldBeNil)

		mux := NewMultiplexer(writePipe, new(testNotifier))
		muxIns := map[string]*MuxIn{}

		demux := &Demultiplexer{In: readPipe}
		demuxOuts := map[string]*RegularCollectionReceiver{}

		inChecksum := map[string]hash.Hash{}
		inLengths := map[string]*int{}

		outChecksum := map[string]hash.Hash{}
		outLengths := map[string]*int{}

		writeErrChan := make(chan error)
		readErrChan := make(chan error)

		makeIns(testIntents, mux, inChecksum, muxIns, inLengths, writeErrChan)
		makeOuts(testIntents, demux, outChecksum, demuxOuts, outLengths, readErrChan)

		go demux.Run()
		go mux.Run()

		for range testIntents {
			err := <-writeErrChan
			So(err, ShouldBeNil)
			err = <-readErrChan
			So(err, ShouldBeNil)
		}
		close(mux.Control)
		muxErr := <-mux.Completed
		So(muxErr, ShouldBeNil)

		for _, dbc := range testIntents {
			ns := dbc.Namespace()
			So(*inLengths[ns], ShouldEqual, *outLengths[ns])
			inSum := inChecksum[ns].Sum([]byte{})
			outSum := outChecksum[ns].Sum([]byte{})
			So(inSum, ShouldResemble, outSum)
		}
	})
	return
}

func makeIns(testIntents []*intents.Intent, mux *Multiplexer, inChecksum map[string]hash.Hash, muxIns map[string]*MuxIn, inLengths map[string]*int, errCh chan<- error) {
	for index, dbc := range testIntents {
		ns := dbc.Namespace()
		sum := crc32.NewIEEE()
		muxIn := &MuxIn{Intent: dbc, Mux: mux}
		inLength := 0

		inChecksum[ns] = sum
		muxIns[ns] = muxIn
		inLengths[ns] = &inLength

		go func(index int) {
			err := muxIn.Open()
			if err != nil {
				errCh <- err
				return
			}
			staticBSONBuf := make([]byte, db.MaxBSONSize)
			for i := 0; i < 10000; i++ {
				bsonBytes, _ := bson.Marshal(testDoc{Bar: index * i, Baz: ns})
				bsonBuf := staticBSONBuf[:len(bsonBytes)]
				copy(bsonBuf, bsonBytes)
				muxIn.Write(bsonBuf)
				sum.Write(bsonBuf)
				inLength += len(bsonBuf)
			}
			err = muxIn.Close()
			errCh <- err
		}(index)
	}
}

func makeOuts(testIntents []*intents.Intent, demux *Demultiplexer, outChecksum map[string]hash.Hash, demuxOuts map[string]*RegularCollectionReceiver, outLengths map[string]*int, errCh chan<- error) {
	for _, dbc := range testIntents {
		ns := dbc.Namespace()
		sum := crc32.NewIEEE()
		muxOut := &RegularCollectionReceiver{
			Intent: dbc,
			Demux:  demux,
			Origin: ns,
		}
		outLength := 0

		outChecksum[ns] = sum
		demuxOuts[ns] = muxOut
		outLengths[ns] = &outLength

		demuxOuts[ns].Open()
		go func() {
			bs := make([]byte, db.MaxBSONSize)
			var err error
			for {
				var length int
				length, err = muxOut.Read(bs)
				if err != nil {
					break
				}
				sum.Write(bs[:length])
				outLength += len(bs[:length])
			}
			if err == io.EOF {
				err = nil
			}
			errCh <- err
		}()
	}
}
