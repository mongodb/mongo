package archive

import (
	"bytes"
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"hash"
	"hash/crc32"
	"io"
	"os"
	"testing"
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

func TestBasicMux(t *testing.T) {
	var err error

	Convey("with 10000 docs in each of five collections", t, func() {
		buf := &closingBuffer{bytes.Buffer{}}

		mux := NewMultiplexer(buf)
		muxIns := map[string]*MuxIn{}

		inChecksum := map[string]hash.Hash{}
		inLength := map[string]int{}
		outChecksum := map[string]hash.Hash{}
		outLength := map[string]int{}

		// To confirm that what we multiplex is the same as what we demultiplex, we
		// create input and output hashes for each namespace. After we finish
		// multiplexing and demultiplexing we will compare all of the CRCs for each
		// namespace
		for _, dbc := range testIntents {
			inChecksum[dbc.Namespace()] = crc32.NewIEEE()
			muxIns[dbc.Namespace()] = &MuxIn{Intent: dbc, Mux: mux}
		}
		errChan := make(chan error)
		for index, dbc := range testIntents {
			closeDbc := dbc
			go func() {
				err = muxIns[closeDbc.Namespace()].Open()
				if err != nil {
					errChan <- err
					return
				}
				staticBSONBuf := make([]byte, db.MaxBSONSize)
				for i := 0; i < 10000; i++ {

					bsonBytes, _ := bson.Marshal(testDoc{Bar: index * i, Baz: closeDbc.Namespace()})
					bsonBuf := staticBSONBuf[:len(bsonBytes)]
					copy(bsonBuf, bsonBytes)
					muxIns[closeDbc.Namespace()].Write(bsonBuf)
					inChecksum[closeDbc.Namespace()].Write(bsonBuf)
					inLength[closeDbc.Namespace()] += len(bsonBuf)
				}
				err = muxIns[closeDbc.Namespace()].Close()
				errChan <- err
			}()
		}
		Convey("each document should be multiplexed", func() {
			fmt.Fprintf(os.Stderr, "About to mux\n")
			go mux.Run()

			for _ = range testIntents {
				err := <-errChan
				So(err, ShouldBeNil)
			}
			close(mux.Control)
			err = <-mux.Completed
			So(err, ShouldBeNil)

			demux := &Demultiplexer{In: buf}
			demuxOuts := map[string]*RegularCollectionReceiver{}

			for _, dbc := range testIntents {
				outChecksum[dbc.Namespace()] = crc32.NewIEEE()
				demuxOuts[dbc.Namespace()] = &RegularCollectionReceiver{Intent: dbc, Demux: demux}
				demuxOuts[dbc.Namespace()].Open()
			}
			errChan := make(chan error)
			for _, dbc := range testIntents {
				closeDbc := dbc
				go func() {
					bs := make([]byte, db.MaxBSONSize)
					var readErr error
					//var length int
					var i int
					for {
						i++
						var length int
						length, readErr = demuxOuts[closeDbc.Namespace()].Read(bs)
						if readErr != nil {
							break
						}
						outChecksum[closeDbc.Namespace()].Write(bs[:length])
						outLength[closeDbc.Namespace()] += len(bs[:length])
					}
					if readErr == io.EOF {
						readErr = nil
					}
					errChan <- readErr
				}()
			}
			Convey("and demultiplexed successfully", func() {
				demux.Run()
				So(err, ShouldBeNil)
				for _ = range testIntents {
					err := <-errChan
					So(err, ShouldBeNil)
				}
				for _, dbc := range testIntents {
					So(inLength[dbc.Namespace()], ShouldEqual, outLength[dbc.Namespace()])
					So(inChecksum[dbc.Namespace()].Sum([]byte{}), ShouldResemble, outChecksum[dbc.Namespace()].Sum([]byte{}))
				}
			})
		})
	})
	return
}

func TestParallelMux(t *testing.T) {

	Convey("parllel mux/demux over a pipe", t, func() {
		readPipe, writePipe, err := os.Pipe()
		So(err, ShouldBeNil)

		mux := NewMultiplexer(writePipe)
		muxIns := map[string]*MuxIn{}

		demux := &Demultiplexer{In: readPipe}
		demuxOuts := map[string]*RegularCollectionReceiver{}

		inChecksum := map[string]hash.Hash{}
		inLength := map[string]int{}

		outChecksum := map[string]hash.Hash{}
		outLength := map[string]int{}

		for _, dbc := range testIntents {
			inChecksum[dbc.Namespace()] = crc32.NewIEEE()
			outChecksum[dbc.Namespace()] = crc32.NewIEEE()

			muxIns[dbc.Namespace()] = &MuxIn{Intent: dbc, Mux: mux}

			demuxOuts[dbc.Namespace()] = &RegularCollectionReceiver{Intent: dbc, Demux: demux}
		}

		writeErrChan := make(chan error)
		readErrChan := make(chan error)

		for index, dbc := range testIntents {
			closeDbc := dbc
			go func() {
				err = muxIns[closeDbc.Namespace()].Open()
				if err != nil {
					writeErrChan <- nil
					return
				}
				staticBSONBuf := make([]byte, db.MaxBSONSize)
				for i := 0; i < 10000; i++ {

					bsonBytes, _ := bson.Marshal(testDoc{Bar: index * i, Baz: closeDbc.Namespace()})
					bsonBuf := staticBSONBuf[:len(bsonBytes)]
					copy(bsonBuf, bsonBytes)
					muxIns[closeDbc.Namespace()].Write(bsonBuf)
					inChecksum[closeDbc.Namespace()].Write(bsonBuf)
					inLength[closeDbc.Namespace()] += len(bsonBuf)
				}

				err = muxIns[closeDbc.Namespace()].Close()
				writeErrChan <- err
			}()
		}

		for _, dbc := range testIntents {
			closeDbc := dbc
			go func() {
				demuxOuts[closeDbc.Namespace()].Open()
				bs := make([]byte, db.MaxBSONSize)
				var readErr error
				//var length int
				var i int
				for {
					i++
					var length int
					length, readErr = demuxOuts[closeDbc.Namespace()].Read(bs)
					if readErr != nil {
						break
					}
					outChecksum[closeDbc.Namespace()].Write(bs[:length])
					outLength[closeDbc.Namespace()] += len(bs[:length])
				}
				if readErr == io.EOF {
					readErr = nil
				}
				readErrChan <- readErr
			}()
		}

		go demux.Run()
		go mux.Run()

		for _ = range testIntents {
			err := <-writeErrChan
			So(err, ShouldBeNil)
			err = <-readErrChan
			So(err, ShouldBeNil)
		}
		close(mux.Control)
		muxErr := <-mux.Completed

		So(muxErr, ShouldBeNil)

		for _, dbc := range testIntents {
			So(inLength[dbc.Namespace()], ShouldEqual, outLength[dbc.Namespace()])
			So(inChecksum[dbc.Namespace()].Sum([]byte{}), ShouldResemble, outChecksum[dbc.Namespace()].Sum([]byte{}))
		}
	})
	return
}
