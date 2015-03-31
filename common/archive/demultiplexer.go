package archive

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"gopkg.in/mgo.v2/bson"
	"io"
)

var errorSuffix string = "dump archive format error"

type Demultiplexer struct {
	in               io.Reader
	outs             map[string]*DemuxOut
	currentNamespace string
	buf              [db.MaxBSONSize]byte
}

func (dmx *Demultiplexer) Run() error {
	parser := Parser{In: dmx.in}
	return parser.ReadAllBlocks(dmx)
}

// HeaderBSON is part of the ParserConsumer interface and recieves headers from parser.
// Its main role is to implement opens and EOF's of the embedded stream
func (dmx *Demultiplexer) HeaderBSON(buf []byte) error {
	colHeader := CollectionHeader{}
	err := bson.Unmarshal(buf, &colHeader)
	if err != nil {
		return fmt.Errorf("%v; header bson doesn't unmarshal as a collection header: (%v)", errorSuffix, err)
	}
	if colHeader.Database == "" {
		return fmt.Errorf("%v; collection header is missing a Database", errorSuffix)
	}
	if colHeader.Collection == "" {
		return fmt.Errorf("%v; collection header is missing a Collection", errorSuffix)
	}
	dmx.currentNamespace = colHeader.Database + "." + colHeader.Collection
	if _, ok := dmx.outs[dmx.currentNamespace]; !ok {
		return fmt.Errorf("%v; collection, %v, header specifies db/collection that shouldn't be in the archive, or is already closed",
			errorSuffix, dmx.currentNamespace, dmx.outs)
	}
	if colHeader.EOF {
		dmx.outs[dmx.currentNamespace].Close()
		delete(dmx.outs, dmx.currentNamespace)
		dmx.currentNamespace = ""
	}
	return nil
}

// End is part of the ParserConsumer interface and recieves the end of archive notification
func (dmx *Demultiplexer) End() error {
	//TODO, check that the outs are all closed here and error if they are not
	if len(dmx.outs) != 0 {
		return fmt.Errorf("%v; archive finished but contained files were unfinished", errorSuffix)
	}
	return nil
}

// End is part of the ParserConsumer interface and recieves BSON bodys from the parser.
// Its main role is to dispatch the body to the Read() function of the current DemuxOut
func (dmx *Demultiplexer) BodyBSON(buf []byte) error {
	if dmx.currentNamespace == "" {
		return fmt.Errorf("%v; collection data without a collection header", errorSuffix)
	}
	// First write a meaningless value to the reader because if the reader writes first
	// then it will panic when the chan is closed. We want the reader to be able to detect
	// that the chan is closed and act accordingly
	dmx.outs[dmx.currentNamespace].readLenChan <- 0
	// Recieve from the reader to put the bytes in to
	readBufChan := <-dmx.outs[dmx.currentNamespace].readBufChan
	if len(readBufChan) > len(buf) {
		return fmt.Errorf("%v; readbuf is not large enough for incomming BodyBSON")
	}
	copy(readBufChan, buf)
	// Send back the length of the data copied in to the buffer
	dmx.outs[dmx.currentNamespace].readLenChan <- len(buf)
	return nil
}

// DemuxOut implements the intent.file interface, which is effectively a ReadWriteCloseOpener
// It is what the intent uses to read data from the demultiplexer
type DemuxOut struct {
	readLenChan chan int
	readBufChan chan []byte
	namespace   string
	demux       *Demultiplexer
}

// Read is part of the intent.file interface
func (dmxOut *DemuxOut) Read(p []byte) (int, error) {
	// Since we're the "reader" here, not the "writer" we need to start with a read, in case the chan is closed
	_, ok := <-dmxOut.readLenChan
	if !ok {
		return 0, io.EOF
	}
	// Send the read buff to the BodyBSON ParserConsumer to fill
	dmxOut.readBufChan <- p
	// Receive the length of data written
	length := <-dmxOut.readLenChan
	return length, nil
}

// Open is part of the intent.file interface
// closing the readLenChan chan will cause the next Read() to return EOF
func (dmxOut *DemuxOut) Close() error {
	close(dmxOut.readLenChan)
	close(dmxOut.readBufChan)
	return nil
}

// Open is part of the intent.file interface
// It creates the chan's in the DemuxOut and adds the DemuxOut to the
// set of DemuxOuts in the demultiplexer
func (dmxOut *DemuxOut) Open() error {
	if dmxOut.demux.outs == nil {
		dmxOut.demux.outs = make(map[string]*DemuxOut)
	}
	dmxOut.readLenChan = make(chan int)
	dmxOut.readBufChan = make(chan []byte)
	// TODO, figure out weather we need to lock around accessing outs
	dmxOut.demux.outs[dmxOut.namespace] = dmxOut
	return nil
}
func (dmxOut *DemuxOut) Write([]byte) (int, error) {
	return 0, nil
}
