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
		BSONPath: "foo.bar",
	},
	&intents.Intent{
		DB:       "ding",
		C:        "bats",
		BSONPath: "ding.bats",
	},
	&intents.Intent{
		DB:       "flim",
		C:        "flam.fooey",
		BSONPath: "flim.flam.fooey",
	},
	&intents.Intent{
		DB:       "crow",
		C:        "bar",
		BSONPath: "crow.bar",
	},
}

type testDoc struct {
	Bar int
	Baz string
}

func TestBasicMux(t *testing.T) {
	var err error

	Convey("with 10000 docs in each of five collections", t, func() {
		buf := &bytes.Buffer{}

		mux := &Multiplexer{Out: buf, Control: make(chan byte)}
		muxIns := map[string]*MuxIn{}

		inChecksum := map[string]hash.Hash{}
		inLength := map[string]int{}
		outChecksum := map[string]hash.Hash{}
		outLength := map[string]int{}

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
					errChan <- nil
					return
				}
				defer muxIns[closeDbc.Namespace()].Close()
				staticBSONBuf := make([]byte, db.MaxBSONSize)
				for i := 0; i < 10000; i++ {

					bsonBytes, _ := bson.Marshal(testDoc{Bar: index * i, Baz: closeDbc.Namespace()})
					bsonBuf := staticBSONBuf[:len(bsonBytes)]
					copy(bsonBuf, bsonBytes)
					muxIns[closeDbc.Namespace()].Write(bsonBuf)
					inChecksum[closeDbc.Namespace()].Write(bsonBuf)
					inLength[closeDbc.Namespace()] += len(bsonBuf)
				}
				errChan <- nil
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
				err = demux.Run()
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
