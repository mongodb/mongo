package mongoreplay

import (
	"container/heap"
	"fmt"
	"sync/atomic"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/tcpassembly"
)

// OpStreamSettings stores settings for any command which may listen to an
// opstream.
type OpStreamSettings struct {
	PcapFile         string `short:"f" description:"path to the pcap file to be read"`
	PacketBufSize    int    `short:"b" description:"Size of heap used to merge separate streams together"`
	CaptureBufSize   int    `long:"capSize" description:"Size in KiB of the PCAP capture buffer"`
	Expression       string `short:"e" long:"expr" description:"BPF filter expression to apply to packets"`
	NetworkInterface string `short:"i" description:"network interface to listen on"`
}

// tcpassembly.Stream implementation.
type stream struct {
	bidi             *bidi
	reassembled      chan []tcpassembly.Reassembly
	reassembly       tcpassembly.Reassembly
	done             chan interface{}
	op               *RawOp
	opTimeStamp      time.Time
	state            streamState
	netFlow, tcpFlow gopacket.Flow
}

// Reassembled receives the new slice of reassembled data and forwards it to the
// MongoOpStream->streamOps goroutine for which turns them in to protocol
// messages.
// Since the tcpassembler reuses the tcpreassembly.Reassembled buffers, we wait
// for streamOps to signal us that it's done with them before returning.
func (stream *stream) Reassembled(reassembly []tcpassembly.Reassembly) {
	stream.reassembled <- reassembly
	<-stream.done
}

// ReassemblyComplete receives from the tcpassembler the fact that the stream is
// now finished. Because our streamOps function may be reading from more then
// one stream, we only shut down the bidi once all the streams are finished.
func (stream *stream) ReassemblyComplete() {

	count := atomic.AddInt32(&stream.bidi.openStreamCount, -1)
	if count < 0 {
		panic("negative openStreamCount")
	}
	if count == 0 {
		stream.bidi.close()
	}
}

// tcpassembly.StreamFactory implementation.

// bidi is a bidirectional connection.
type bidi struct {
	streams          [2]*stream
	openStreamCount  int32
	opStream         *MongoOpStream
	responseStream   bool
	sawStart         bool
	connectionNumber int64
}

func newBidi(netFlow, tcpFlow gopacket.Flow, opStream *MongoOpStream, num int64) *bidi {
	bidi := &bidi{connectionNumber: num}
	bidi.streams[0] = &stream{
		bidi:        bidi,
		reassembled: make(chan []tcpassembly.Reassembly),
		done:        make(chan interface{}),
		op:          &RawOp{},
		netFlow:     netFlow,
		tcpFlow:     tcpFlow,
	}
	bidi.streams[1] = &stream{
		bidi:        bidi,
		reassembled: make(chan []tcpassembly.Reassembly),
		done:        make(chan interface{}),
		op:          &RawOp{},
		netFlow:     netFlow.Reverse(),
		tcpFlow:     tcpFlow.Reverse(),
	}
	bidi.opStream = opStream
	return bidi
}

func (bidi *bidi) logvf(minVerb int, format string, a ...interface{}) {
	userInfoLogger.Logvf(minVerb, "stream %v %v", bidi.connectionNumber, fmt.Sprintf(format, a...))
}

// close closes the channels used to communicate between the
// stream's and bidi.streamOps,
// and removes the bidi from the bidiMap.
func (bidi *bidi) close() {
	close(bidi.streams[0].reassembled)
	close(bidi.streams[0].done)
	close(bidi.streams[1].reassembled)
	close(bidi.streams[1].done)
	delete(bidi.opStream.bidiMap, bidiKey{bidi.streams[1].netFlow, bidi.streams[1].tcpFlow})
	delete(bidi.opStream.bidiMap, bidiKey{bidi.streams[0].netFlow, bidi.streams[0].tcpFlow})
	// probably not important, just trying to make the garbage collection easier.
	bidi.streams[0].bidi = nil
	bidi.streams[1].bidi = nil
}

type bidiKey struct {
	net, transport gopacket.Flow
}

// MongoOpStream is the opstream which yields RecordedOps
type MongoOpStream struct {
	Ops chan *RecordedOp

	FirstSeen         time.Time
	unorderedOps      chan RecordedOp
	opHeap            *orderedOps
	bidiMap           map[bidiKey]*bidi
	connectionCounter chan int64
	connectionNumber  int64
}

// NewMongoOpStream initializes a new MongoOpStream
func NewMongoOpStream(heapBufSize int) *MongoOpStream {
	h := make(orderedOps, 0, heapBufSize)
	os := &MongoOpStream{
		Ops:               make(chan *RecordedOp), // ordered
		unorderedOps:      make(chan RecordedOp),  // unordered
		opHeap:            &h,
		bidiMap:           make(map[bidiKey]*bidi),
		connectionCounter: make(chan int64, 1024),
	}
	heap.Init(os.opHeap)
	go func() {
		var counter int64
		for {
			os.connectionCounter <- counter
			counter++
		}
	}()
	go os.handleOps()
	return os
}

// New is the factory method called by the tcpassembly to generate new tcpassembly.Stream.
func (os *MongoOpStream) New(netFlow, tcpFlow gopacket.Flow) tcpassembly.Stream {
	key := bidiKey{netFlow, tcpFlow}
	rkey := bidiKey{netFlow.Reverse(), tcpFlow.Reverse()}
	if bidi, ok := os.bidiMap[key]; ok {
		atomic.AddInt32(&bidi.openStreamCount, 1)
		delete(os.bidiMap, key)
		return bidi.streams[1]
	}
	bidi := newBidi(netFlow, tcpFlow, os, <-os.connectionCounter)
	os.bidiMap[rkey] = bidi
	atomic.AddInt32(&bidi.openStreamCount, 1)
	go bidi.streamOps()
	return bidi.streams[0]
}

// Close is called by the tcpassembly to indicate that all of the packets
// have been processed.
func (os *MongoOpStream) Close() error {
	close(os.unorderedOps)
	os.unorderedOps = nil
	return nil
}

// SetFirstSeen sets the time for the first message on the MongoOpStream.
// All of this SetFirstSeen/FirstSeen/SetFirstseer stuff can go away ( from here
// and from packet_handler.go ) it's a cruft and was how someone was trying to
// get around the fact that using the tcpassembly.tcpreader library throws away
// all of the metadata about the stream.
func (os *MongoOpStream) SetFirstSeen(t time.Time) {
	os.FirstSeen = t
}

// handleOps runs all of the ops read from the unorderedOps through a heapsort
// and then runs them out on the Ops channel.
func (os *MongoOpStream) handleOps() {
	defer close(os.Ops)
	var counter int64
	for op := range os.unorderedOps {
		heap.Push(os.opHeap, op)
		if len(*os.opHeap) == cap(*os.opHeap) {
			nextOp := heap.Pop(os.opHeap).(RecordedOp)
			counter++
			nextOp.Order = counter
			os.Ops <- &nextOp
		}
	}
	for len(*os.opHeap) > 0 {
		nextOp := heap.Pop(os.opHeap).(RecordedOp)
		counter++
		nextOp.Order = counter
		os.Ops <- &nextOp
	}
}

type streamState int

func (st streamState) String() string {
	switch st {
	case streamStateBeforeMessage:
		return "Before Message"
	case streamStateInMessage:
		return "In Message"
	case streamStateOutOfSync:
		return "Out Of Sync"
	}
	return "Unknown"
}

const (
	streamStateBeforeMessage streamState = iota
	streamStateInMessage
	streamStateOutOfSync
)

func (bidi *bidi) handleStreamStateBeforeMessage(stream *stream) {
	if stream.reassembly.Start {
		if bidi.sawStart {
			panic("apparently saw the beginning of a connection twice")
		}
		bidi.sawStart = true
	}
	// TODO deal with the situation that the first packet doesn't contain a
	// whole MessageHeader of an otherwise valid protocol message.  The
	// following code erroneously assumes that all packets will have at least 16
	// bytes of data
	if len(stream.reassembly.Bytes) < 16 {
		stream.state = streamStateOutOfSync
		stream.reassembly.Bytes = stream.reassembly.Bytes[:0]
		return
	}
	stream.op.Header.FromWire(stream.reassembly.Bytes)
	if !stream.op.Header.LooksReal() {
		// When we're here and stream.reassembly.Start is true we may be able to
		// know that we're actually not looking at mongodb traffic and that this
		// whole stream should be discarded.
		bidi.logvf(DebugLow, "not a good header %#v", stream.op.Header)
		bidi.logvf(Info, "Expected to, but didn't see a valid protocol message")
		stream.state = streamStateOutOfSync
		stream.reassembly.Bytes = stream.reassembly.Bytes[:0]
		return
	}
	stream.op.Body = make([]byte, 16, stream.op.Header.MessageLength)
	stream.state = streamStateInMessage
	stream.opTimeStamp = stream.reassembly.Seen
	copy(stream.op.Body, stream.reassembly.Bytes)
	stream.reassembly.Bytes = stream.reassembly.Bytes[16:]
	return
}
func (bidi *bidi) handleStreamStateInMessage(stream *stream) {
	var copySize int
	bodyLen := len(stream.op.Body)
	if bodyLen+len(stream.reassembly.Bytes) > int(stream.op.Header.MessageLength) {
		copySize = int(stream.op.Header.MessageLength) - bodyLen
	} else {
		copySize = len(stream.reassembly.Bytes)
	}
	stream.op.Body = stream.op.Body[:bodyLen+copySize]
	copied := copy(stream.op.Body[bodyLen:], stream.reassembly.Bytes)
	if copied != copySize {
		panic("copied != copySize")
	}
	stream.reassembly.Bytes = stream.reassembly.Bytes[copySize:]
	if len(stream.op.Body) == int(stream.op.Header.MessageLength) {
		// TODO maybe remember if we were recently in streamStateOutOfSync,
		// and if so, parse the raw op here.

		bidi.opStream.unorderedOps <- RecordedOp{
			RawOp:             *stream.op,
			Seen:              &PreciseTime{stream.opTimeStamp},
			SrcEndpoint:       stream.netFlow.Src().String(),
			DstEndpoint:       stream.netFlow.Dst().String(),
			SeenConnectionNum: bidi.connectionNumber,
		}

		stream.op = &RawOp{}
		stream.state = streamStateBeforeMessage
		if len(stream.reassembly.Bytes) > 0 {
			// parse the remainder of the stream.reassembly as a new message.
			return
		}
	}
	return
}
func (bidi *bidi) handleStreamStateOutOfSync(stream *stream) {
	bidi.logvf(DebugHigh, "out of sync")
	if len(stream.reassembly.Bytes) < 16 {
		stream.reassembly.Bytes = stream.reassembly.Bytes[:0]
		return
	}
	stream.op.Header.FromWire(stream.reassembly.Bytes)
	bidi.logvf(DebugHigh, "possible message header %#v", stream.op.Header)
	if stream.op.Header.LooksReal() {
		stream.state = streamStateBeforeMessage
		bidi.logvf(DebugLow, "synchronized")
		return
	}
	stream.reassembly.Bytes = stream.reassembly.Bytes[:0]
	return
}

// streamOps reads tcpassembly.Reassembly[] blocks from the
// stream's and tries to create whole protocol messages from them.
func (bidi *bidi) streamOps() {
	bidi.logvf(Info, "starting")
	for {
		var reassemblies []tcpassembly.Reassembly
		var reassembliesStream int
		var ok bool
		select {
		case reassemblies, ok = <-bidi.streams[0].reassembled:
			reassembliesStream = 0
		case reassemblies, ok = <-bidi.streams[1].reassembled:
			reassembliesStream = 1
		}
		if !ok {
			break
		}
		stream := bidi.streams[reassembliesStream]

		for _, stream.reassembly = range reassemblies {
			// Skip > 0 means that we've missed something, and we have
			// incomplete packets in hand.
			if stream.reassembly.Skip > 0 {
				// TODO, we may want to do more state specific reporting here.
				stream.state = streamStateOutOfSync
				//when we have skip, we destroy this buffer
				stream.op.Body = stream.op.Body[:0]
				bidi.logvf(Info, "Connection %v state '%v': ignoring incomplete packet (skip: %v)", bidi.connectionNumber, stream.state, stream.reassembly.Skip)
				continue
			}
			// Skip < 0 means that we're picking up a stream mid-stream, and we
			// don't really know the state of what's in hand, we need to
			// synchronize.
			if stream.reassembly.Skip < 0 {
				bidi.logvf(Info, "Connection %v state '%v': capture started in the middle of stream", bidi.connectionNumber, stream.state)
				stream.state = streamStateOutOfSync
			}

			for len(stream.reassembly.Bytes) > 0 {
				bidi.logvf(DebugHigh, "Connection %v: state '%v'", bidi.connectionNumber, stream.state)
				switch stream.state {
				case streamStateBeforeMessage:
					bidi.handleStreamStateBeforeMessage(stream)
				case streamStateInMessage:
					bidi.handleStreamStateInMessage(stream)
				case streamStateOutOfSync:
					bidi.handleStreamStateOutOfSync(stream)
				}
			}
		}
		// inform the tcpassembly that we've finished with the reassemblies.
		stream.done <- nil
	}
	bidi.logvf(Info, "Connection %v: finishing", bidi.connectionNumber)
}
