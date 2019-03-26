// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// Package reassembly provides TCP stream re-assembly.
//
// The reassembly package implements uni-directional TCP reassembly, for use in
// packet-sniffing applications.  The caller reads packets off the wire, then
// presents them to an Assembler in the form of gopacket layers.TCP packets
// (github.com/google/gopacket, github.com/google/gopacket/layers).
//
// The Assembler uses a user-supplied
// StreamFactory to create a user-defined Stream interface, then passes packet
// data in stream order to that object.  A concurrency-safe StreamPool keeps
// track of all current Streams being reassembled, so multiple Assemblers may
// run at once to assemble packets while taking advantage of multiple cores.
//
// TODO: Add simplest example
package reassembly

import (
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"sync"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

// TODO:
// - push to Stream on Ack
// - implement chunked (cheap) reads and Reader() interface
// - better organize file: split files: 'mem', 'misc' (seq + flow)

var defaultDebug = false

var debugLog = flag.Bool("assembly_debug_log", defaultDebug, "If true, the github.com/google/gopacket/reassembly library will log verbose debugging information (at least one line per packet)")

const invalidSequence = -1
const uint32Max = 0xFFFFFFFF

// Sequence is a TCP sequence number.  It provides a few convenience functions
// for handling TCP wrap-around.  The sequence should always be in the range
// [0,0xFFFFFFFF]... its other bits are simply used in wrap-around calculations
// and should never be set.
type Sequence int64

// Difference defines an ordering for comparing TCP sequences that's safe for
// roll-overs.  It returns:
//    > 0 : if t comes after s
//    < 0 : if t comes before s
//      0 : if t == s
// The number returned is the sequence difference, so 4.Difference(8) will
// return 4.
//
// It handles rollovers by considering any sequence in the first quarter of the
// uint32 space to be after any sequence in the last quarter of that space, thus
// wrapping the uint32 space.
func (s Sequence) Difference(t Sequence) int {
	if s > uint32Max-uint32Max/4 && t < uint32Max/4 {
		t += uint32Max
	} else if t > uint32Max-uint32Max/4 && s < uint32Max/4 {
		s += uint32Max
	}
	return int(t - s)
}

// Add adds an integer to a sequence and returns the resulting sequence.
func (s Sequence) Add(t int) Sequence {
	return (s + Sequence(t)) & uint32Max
}

// TCPAssemblyStats provides some figures for a ScatterGather
type TCPAssemblyStats struct {
	// For this ScatterGather
	Chunks  int
	Packets int
	// For the half connection, since last call to ReassembledSG()
	QueuedBytes    int
	QueuedPackets  int
	OverlapBytes   int
	OverlapPackets int
}

// ScatterGather is used to pass reassembled data and metadata of reassembled
// packets to a Stream via ReassembledSG
type ScatterGather interface {
	// Returns the length of available bytes and saved bytes
	Lengths() (int, int)
	// Returns the bytes up to length (shall be <= available bytes)
	Fetch(length int) []byte
	// Tell to keep from offset
	KeepFrom(offset int)
	// Return CaptureInfo of packet corresponding to given offset
	CaptureInfo(offset int) gopacket.CaptureInfo
	// Return some info about the reassembled chunks
	Info() (direction TCPFlowDirection, start bool, end bool, skip int)
	// Return some stats regarding the state of the stream
	Stats() TCPAssemblyStats
}

// byteContainer is either a page or a livePacket
type byteContainer interface {
	getBytes() []byte
	length() int
	convertToPages(*pageCache, int, AssemblerContext) (*page, *page, int)
	captureInfo() gopacket.CaptureInfo
	assemblerContext() AssemblerContext
	release(*pageCache) int
	isStart() bool
	isEnd() bool
	getSeq() Sequence
	isPacket() bool
}

// Implements a ScatterGather
type reassemblyObject struct {
	all       []byteContainer
	Skip      int
	Direction TCPFlowDirection
	saved     int
	toKeep    int
	// stats
	queuedBytes    int
	queuedPackets  int
	overlapBytes   int
	overlapPackets int
}

func (rl *reassemblyObject) Lengths() (int, int) {
	l := 0
	for _, r := range rl.all {
		l += r.length()
	}
	return l, rl.saved
}

func (rl *reassemblyObject) Fetch(l int) []byte {
	if l <= rl.all[0].length() {
		return rl.all[0].getBytes()[:l]
	}
	bytes := make([]byte, 0, l)
	for _, bc := range rl.all {
		bytes = append(bytes, bc.getBytes()...)
	}
	return bytes[:l]
}

func (rl *reassemblyObject) KeepFrom(offset int) {
	rl.toKeep = offset
}

func (rl *reassemblyObject) CaptureInfo(offset int) gopacket.CaptureInfo {
	current := 0
	var r byteContainer
	for _, r = range rl.all {
		if current >= offset {
			return r.captureInfo()
		}
		current += r.length()
	}
	if r != nil && current >= offset {
		return r.captureInfo()
	}
	// Invalid offset
	return gopacket.CaptureInfo{}
}

func (rl *reassemblyObject) Info() (TCPFlowDirection, bool, bool, int) {
	return rl.Direction, rl.all[0].isStart(), rl.all[len(rl.all)-1].isEnd(), rl.Skip
}

func (rl *reassemblyObject) Stats() TCPAssemblyStats {
	packets := int(0)
	for _, r := range rl.all {
		if r.isPacket() {
			packets++
		}
	}
	return TCPAssemblyStats{
		Chunks:         len(rl.all),
		Packets:        packets,
		QueuedBytes:    rl.queuedBytes,
		QueuedPackets:  rl.queuedPackets,
		OverlapBytes:   rl.overlapBytes,
		OverlapPackets: rl.overlapPackets,
	}
}

const pageBytes = 1900

// TCPFlowDirection distinguish the two half-connections directions.
//
// TCPDirClientToServer is assigned to half-connection for the first received
// packet, hence might be wrong if packets are not received in order.
// It's up to the caller (e.g. in Accept()) to decide if the direction should
// be interpretted differently.
type TCPFlowDirection bool

// Value are not really useful
const (
	TCPDirClientToServer TCPFlowDirection = false
	TCPDirServerToClient TCPFlowDirection = true
)

func (dir TCPFlowDirection) String() string {
	switch dir {
	case TCPDirClientToServer:
		return "client->server"
	case TCPDirServerToClient:
		return "server->client"
	}
	return ""
}

// Reverse returns the reversed direction
func (dir TCPFlowDirection) Reverse() TCPFlowDirection {
	return !dir
}

/* page: implements a byteContainer */

// page is used to store TCP data we're not ready for yet (out-of-order
// packets).  Unused pages are stored in and returned from a pageCache, which
// avoids memory allocation.  Used pages are stored in a doubly-linked list in
// a connection.
type page struct {
	bytes      []byte
	seq        Sequence
	prev, next *page
	buf        [pageBytes]byte
	ac         AssemblerContext // only set for the first page of a packet
	seen       time.Time
	start, end bool
}

func (p *page) getBytes() []byte {
	return p.bytes
}
func (p *page) captureInfo() gopacket.CaptureInfo {
	return p.ac.GetCaptureInfo()
}
func (p *page) assemblerContext() AssemblerContext {
	return p.ac
}
func (p *page) convertToPages(pc *pageCache, skip int, ac AssemblerContext) (*page, *page, int) {
	if skip != 0 {
		p.bytes = p.bytes[skip:]
		p.seq = p.seq.Add(skip)
	}
	p.prev, p.next = nil, nil
	return p, p, 1
}
func (p *page) length() int {
	return len(p.bytes)
}
func (p *page) release(pc *pageCache) int {
	pc.replace(p)
	return 1
}
func (p *page) isStart() bool {
	return p.start
}
func (p *page) isEnd() bool {
	return p.end
}
func (p *page) getSeq() Sequence {
	return p.seq
}
func (p *page) isPacket() bool {
	return p.ac != nil
}
func (p *page) String() string {
	return fmt.Sprintf("page@%p{seq: %v, bytes:%d, -> nextSeq:%v} (prev:%p, next:%p)", p, p.seq, len(p.bytes), p.seq+Sequence(len(p.bytes)), p.prev, p.next)
}

/* livePacket: implements a byteContainer */
type livePacket struct {
	bytes []byte
	start bool
	end   bool
	ci    gopacket.CaptureInfo
	ac    AssemblerContext
	seq   Sequence
}

func (lp *livePacket) getBytes() []byte {
	return lp.bytes
}
func (lp *livePacket) captureInfo() gopacket.CaptureInfo {
	return lp.ci
}
func (lp *livePacket) assemblerContext() AssemblerContext {
	return lp.ac
}
func (lp *livePacket) length() int {
	return len(lp.bytes)
}
func (lp *livePacket) isStart() bool {
	return lp.start
}
func (lp *livePacket) isEnd() bool {
	return lp.end
}
func (lp *livePacket) getSeq() Sequence {
	return lp.seq
}
func (lp *livePacket) isPacket() bool {
	return true
}

// Creates a page (or set of pages) from a TCP packet: returns the first and last
// page in its doubly-linked list of new pages.
func (lp *livePacket) convertToPages(pc *pageCache, skip int, ac AssemblerContext) (*page, *page, int) {
	ts := lp.ci.Timestamp
	first := pc.next(ts)
	current := first
	current.prev = nil
	first.ac = ac
	numPages := 1
	seq, bytes := lp.seq.Add(skip), lp.bytes[skip:]
	for {
		length := min(len(bytes), pageBytes)
		current.bytes = current.buf[:length]
		copy(current.bytes, bytes)
		current.seq = seq
		bytes = bytes[length:]
		if len(bytes) == 0 {
			current.end = lp.isEnd()
			current.next = nil
			break
		}
		seq = seq.Add(length)
		current.next = pc.next(ts)
		current.next.prev = current
		current = current.next
		current.ac = nil
		numPages++
	}
	return first, current, numPages
}
func (lp *livePacket) estimateNumberOfPages() int {
	return (len(lp.bytes) + pageBytes + 1) / pageBytes
}

func (lp *livePacket) release(*pageCache) int {
	return 0
}

// Stream is implemented by the caller to handle incoming reassembled
// TCP data.  Callers create a StreamFactory, then StreamPool uses
// it to create a new Stream for every TCP stream.
//
// assembly will, in order:
//    1) Create the stream via StreamFactory.New
//    2) Call ReassembledSG 0 or more times, passing in reassembled TCP data in order
//    3) Call ReassemblyComplete one time, after which the stream is dereferenced by assembly.
type Stream interface {
	// Tell whether the TCP packet should be accepted, start could be modified to force a start even if no SYN have been seen
	Accept(tcp *layers.TCP, ci gopacket.CaptureInfo, dir TCPFlowDirection, nextSeq Sequence, start *bool, ac AssemblerContext) bool

	// ReassembledSG is called zero or more times.
	// ScatterGather is reused after each Reassembled call,
	// so it's important to copy anything you need out of it,
	// especially bytes (or use KeepFrom())
	ReassembledSG(sg ScatterGather, ac AssemblerContext)

	// ReassemblyComplete is called when assembly decides there is
	// no more data for this Stream, either because a FIN or RST packet
	// was seen, or because the stream has timed out without any new
	// packet data (due to a call to FlushCloseOlderThan).
	// It should return true if the connection should be removed from the pool
	// It can return false if it want to see subsequent packets with Accept(), e.g. to
	// see FIN-ACK, for deeper state-machine analysis.
	ReassemblyComplete(ac AssemblerContext) bool
}

// StreamFactory is used by assembly to create a new stream for each
// new TCP session.
type StreamFactory interface {
	// New should return a new stream for the given TCP key.
	New(netFlow, tcpFlow gopacket.Flow, tcp *layers.TCP, ac AssemblerContext) Stream
}

type key [2]gopacket.Flow

func (k *key) String() string {
	return fmt.Sprintf("%s:%s", k[0], k[1])
}

func (k *key) Reverse() key {
	return key{
		k[0].Reverse(),
		k[1].Reverse(),
	}
}

const assemblerReturnValueInitialSize = 16

/* one-way connection, i.e. halfconnection */
type halfconnection struct {
	dir               TCPFlowDirection
	pages             int      // Number of pages used (both in first/last and saved)
	saved             *page    // Doubly-linked list of in-order pages (seq < nextSeq) already given to Stream who told us to keep
	first, last       *page    // Doubly-linked list of out-of-order pages (seq > nextSeq)
	nextSeq           Sequence // sequence number of in-order received bytes
	ackSeq            Sequence
	created, lastSeen time.Time
	stream            Stream
	closed            bool
	// for stats
	queuedBytes    int
	queuedPackets  int
	overlapBytes   int
	overlapPackets int
}

func (half *halfconnection) String() string {
	closed := ""
	if half.closed {
		closed = "closed "
	}
	return fmt.Sprintf("%screated:%v, last:%v", closed, half.created, half.lastSeen)
}

// Dump returns a string (crypticly) describing the halfconnction
func (half *halfconnection) Dump() string {
	s := fmt.Sprintf("pages: %d\n"+
		"nextSeq: %d\n"+
		"ackSeq: %d\n"+
		"Seen :  %s\n"+
		"dir:    %s\n", half.pages, half.nextSeq, half.ackSeq, half.lastSeen, half.dir)
	nb := 0
	for p := half.first; p != nil; p = p.next {
		s += fmt.Sprintf("	Page[%d] %s len: %d\n", nb, p, len(p.bytes))
		nb++
	}
	return s
}

/* Bi-directionnal connection */

type connection struct {
	key      key // client->server
	c2s, s2c halfconnection
	mu       sync.Mutex
}

func (c *connection) reset(k key, s Stream, ts time.Time) {
	c.key = k
	base := halfconnection{
		nextSeq:  invalidSequence,
		ackSeq:   invalidSequence,
		created:  ts,
		lastSeen: ts,
		stream:   s,
	}
	c.c2s, c.s2c = base, base
	c.c2s.dir, c.s2c.dir = TCPDirClientToServer, TCPDirServerToClient
}

func (c *connection) lastSeen() time.Time {
	if c.c2s.lastSeen.Before(c.s2c.lastSeen) {
		return c.s2c.lastSeen
	}

	return c.c2s.lastSeen
}

func (c *connection) String() string {
	return fmt.Sprintf("c2s: %s, s2c: %s", &c.c2s, &c.s2c)
}

/*
 * Assembler
 */

// DefaultAssemblerOptions provides default options for an assembler.
// These options are used by default when calling NewAssembler, so if
// modified before a NewAssembler call they'll affect the resulting Assembler.
//
// Note that the default options can result in ever-increasing memory usage
// unless one of the Flush* methods is called on a regular basis.
var DefaultAssemblerOptions = AssemblerOptions{
	MaxBufferedPagesPerConnection: 0, // unlimited
	MaxBufferedPagesTotal:         0, // unlimited
}

// AssemblerOptions controls the behavior of each assembler.  Modify the
// options of each assembler you create to change their behavior.
type AssemblerOptions struct {
	// MaxBufferedPagesTotal is an upper limit on the total number of pages to
	// buffer while waiting for out-of-order packets.  Once this limit is
	// reached, the assembler will degrade to flushing every connection it
	// gets a packet for.  If <= 0, this is ignored.
	MaxBufferedPagesTotal int
	// MaxBufferedPagesPerConnection is an upper limit on the number of pages
	// buffered for a single connection.  Should this limit be reached for a
	// particular connection, the smallest sequence number will be flushed, along
	// with any contiguous data.  If <= 0, this is ignored.
	MaxBufferedPagesPerConnection int
}

// Assembler handles reassembling TCP streams.  It is not safe for
// concurrency... after passing a packet in via the Assemble call, the caller
// must wait for that call to return before calling Assemble again.  Callers can
// get around this by creating multiple assemblers that share a StreamPool.  In
// that case, each individual stream will still be handled serially (each stream
// has an individual mutex associated with it), however multiple assemblers can
// assemble different connections concurrently.
//
// The Assembler provides (hopefully) fast TCP stream re-assembly for sniffing
// applications written in Go.  The Assembler uses the following methods to be
// as fast as possible, to keep packet processing speedy:
//
// Avoids Lock Contention
//
// Assemblers locks connections, but each connection has an individual lock, and
// rarely will two Assemblers be looking at the same connection.  Assemblers
// lock the StreamPool when looking up connections, but they use Reader
// locks initially, and only force a write lock if they need to create a new
// connection or close one down.  These happen much less frequently than
// individual packet handling.
//
// Each assembler runs in its own goroutine, and the only state shared between
// goroutines is through the StreamPool.  Thus all internal Assembler state
// can be handled without any locking.
//
// NOTE:  If you can guarantee that packets going to a set of Assemblers will
// contain information on different connections per Assembler (for example,
// they're already hashed by PF_RING hashing or some other hashing mechanism),
// then we recommend you use a seperate StreamPool per Assembler, thus
// avoiding all lock contention.  Only when different Assemblers could receive
// packets for the same Stream should a StreamPool be shared between them.
//
// Avoids Memory Copying
//
// In the common case, handling of a single TCP packet should result in zero
// memory allocations.  The Assembler will look up the connection, figure out
// that the packet has arrived in order, and immediately pass that packet on to
// the appropriate connection's handling code.  Only if a packet arrives out of
// order is its contents copied and stored in memory for later.
//
// Avoids Memory Allocation
//
// Assemblers try very hard to not use memory allocation unless absolutely
// necessary.  Packet data for sequential packets is passed directly to streams
// with no copying or allocation.  Packet data for out-of-order packets is
// copied into reusable pages, and new pages are only allocated rarely when the
// page cache runs out.  Page caches are Assembler-specific, thus not used
// concurrently and requiring no locking.
//
// Internal representations for connection objects are also reused over time.
// Because of this, the most common memory allocation done by the Assembler is
// generally what's done by the caller in StreamFactory.New.  If no allocation
// is done there, then very little allocation is done ever, mostly to handle
// large increases in bandwidth or numbers of connections.
//
// TODO:  The page caches used by an Assembler will grow to the size necessary
// to handle a workload, and currently will never shrink.  This means that
// traffic spikes can result in large memory usage which isn't garbage
// collected when typical traffic levels return.
type Assembler struct {
	AssemblerOptions
	ret      []byteContainer
	pc       *pageCache
	connPool *StreamPool
	cacheLP  livePacket
	cacheSG  reassemblyObject
	start    bool
}

// NewAssembler creates a new assembler.  Pass in the StreamPool
// to use, may be shared across assemblers.
//
// This sets some sane defaults for the assembler options,
// see DefaultAssemblerOptions for details.
func NewAssembler(pool *StreamPool) *Assembler {
	pool.mu.Lock()
	pool.users++
	pool.mu.Unlock()
	return &Assembler{
		ret:              make([]byteContainer, 0, assemblerReturnValueInitialSize),
		pc:               newPageCache(),
		connPool:         pool,
		AssemblerOptions: DefaultAssemblerOptions,
	}
}

// Dump returns a short string describing the page usage of the Assembler
func (a *Assembler) Dump() string {
	s := ""
	s += fmt.Sprintf("pageCache: used: %d, size: %d, free: %d", a.pc.used, a.pc.size, len(a.pc.free))
	return s
}

// AssemblerContext provides method to get metadata
type AssemblerContext interface {
	GetCaptureInfo() gopacket.CaptureInfo
}

// Implements AssemblerContext for Assemble()
type assemblerSimpleContext gopacket.CaptureInfo

func (asc *assemblerSimpleContext) GetCaptureInfo() gopacket.CaptureInfo {
	return gopacket.CaptureInfo(*asc)
}

// Assemble calls AssembleWithContext with the current timestamp, useful for
// packets being read directly off the wire.
func (a *Assembler) Assemble(netFlow gopacket.Flow, t *layers.TCP) {
	ctx := assemblerSimpleContext(gopacket.CaptureInfo{Timestamp: time.Now()})
	a.AssembleWithContext(netFlow, t, &ctx)
}

type assemblerAction struct {
	nextSeq Sequence
	queue   bool
}

// AssembleWithContext reassembles the given TCP packet into its appropriate
// stream.
//
// The timestamp passed in must be the timestamp the packet was seen.
// For packets read off the wire, time.Now() should be fine.  For packets read
// from PCAP files, CaptureInfo.Timestamp should be passed in.  This timestamp
// will affect which streams are flushed by a call to FlushCloseOlderThan.
//
// Each AssembleWithContext call results in, in order:
//
//    zero or one call to StreamFactory.New, creating a stream
//    zero or one call to ReassembledSG on a single stream
//    zero or one call to ReassemblyComplete on the same stream
func (a *Assembler) AssembleWithContext(netFlow gopacket.Flow, t *layers.TCP, ac AssemblerContext) {
	var conn *connection
	var half *halfconnection
	var rev *halfconnection

	a.ret = a.ret[:0]
	key := key{netFlow, t.TransportFlow()}
	ci := ac.GetCaptureInfo()
	timestamp := ci.Timestamp

	conn, half, rev = a.connPool.getConnection(key, false, timestamp, t, ac)
	if conn == nil {
		if *debugLog {
			log.Printf("%v got empty packet on otherwise empty connection", key)
		}
		return
	}
	conn.mu.Lock()
	defer conn.mu.Unlock()
	if half.lastSeen.Before(timestamp) {
		half.lastSeen = timestamp
	}
	a.start = half.nextSeq == invalidSequence && t.SYN
	if *debugLog {
		if half.nextSeq < rev.ackSeq {
			log.Printf("Delay detected on %v, data is acked but not assembled yet (acked %v, nextSeq %v)", key, rev.ackSeq, half.nextSeq)
		}
	}

	if !half.stream.Accept(t, ci, half.dir, half.nextSeq, &a.start, ac) {
		if *debugLog {
			log.Printf("Ignoring packet")
		}
		return
	}
	if half.closed {
		// this way is closed
		if *debugLog {
			log.Printf("%v got packet on closed half", key)
		}
		return
	}

	seq, ack, bytes := Sequence(t.Seq), Sequence(t.Ack), t.Payload
	if t.ACK {
		half.ackSeq = ack
	}
	// TODO: push when Ack is seen ??
	action := assemblerAction{
		nextSeq: Sequence(invalidSequence),
		queue:   true,
	}
	a.dump("AssembleWithContext()", half)
	if half.nextSeq == invalidSequence {
		if t.SYN {
			if *debugLog {
				log.Printf("%v saw first SYN packet, returning immediately, seq=%v", key, seq)
			}
			seq = seq.Add(1)
			half.nextSeq = seq
			action.queue = false
		} else if a.start {
			if *debugLog {
				log.Printf("%v start forced", key)
			}
			half.nextSeq = seq
			action.queue = false
		} else {
			if *debugLog {
				log.Printf("%v waiting for start, storing into connection", key)
			}
		}
	} else {
		diff := half.nextSeq.Difference(seq)
		if diff > 0 {
			if *debugLog {
				log.Printf("%v gap in sequence numbers (%v, %v) diff %v, storing into connection", key, half.nextSeq, seq, diff)
			}
		} else {
			if *debugLog {
				log.Printf("%v found contiguous data (%v, %v), returning immediately: len:%d", key, seq, half.nextSeq, len(bytes))
			}
			action.queue = false
		}
	}

	action = a.handleBytes(bytes, seq, half, ci, t.SYN, t.RST || t.FIN, action, ac)
	if len(a.ret) > 0 {
		action.nextSeq = a.sendToConnection(conn, half, ac)
	}
	if action.nextSeq != invalidSequence {
		half.nextSeq = action.nextSeq
		if t.FIN {
			half.nextSeq = half.nextSeq.Add(1)
		}
	}
	if *debugLog {
		log.Printf("%v nextSeq:%d", key, half.nextSeq)
	}
}

// Overlap strategies:
//  - new packet overlaps with sent packets:
//	1) discard new overlapping part
//	2) overwrite old overlapped (TODO)
//  - new packet overlaps existing queued packets:
//	a) consider "age" by timestamp (TODO)
//	b) consider "age" by being present
//	Then
//      1) discard new overlapping part
//      2) overwrite queued part

func (a *Assembler) checkOverlap(half *halfconnection, queue bool, ac AssemblerContext) {
	var next *page
	cur := half.last
	bytes := a.cacheLP.bytes
	start := a.cacheLP.seq
	end := start.Add(len(bytes))

	a.dump("before checkOverlap", half)

	//          [s6           :           e6]
	//   [s1:e1][s2:e2] -- [s3:e3] -- [s4:e4][s5:e5]
	//             [s <--ds-- : --de--> e]
	for cur != nil {

		if *debugLog {
			log.Printf("cur = %p (%s)\n", cur, cur)
		}

		// end < cur.start: continue (5)
		if end.Difference(cur.seq) > 0 {
			if *debugLog {
				log.Printf("case 5\n")
			}
			next = cur
			cur = cur.prev
			continue
		}

		curEnd := cur.seq.Add(len(cur.bytes))
		// start > cur.end: stop (1)
		if start.Difference(curEnd) <= 0 {
			if *debugLog {
				log.Printf("case 1\n")
			}
			break
		}

		diffStart := start.Difference(cur.seq)
		diffEnd := end.Difference(curEnd)

		// end > cur.end && start < cur.start: drop (3)
		if diffEnd <= 0 && diffStart >= 0 {
			if *debugLog {
				log.Printf("case 3\n")
			}
			if cur.isPacket() {
				half.overlapPackets++
			}
			half.overlapBytes += len(cur.bytes)
			// update links
			if cur.prev != nil {
				cur.prev.next = cur.next
			} else {
				half.first = cur.next
			}
			if cur.next != nil {
				cur.next.prev = cur.prev
			} else {
				half.last = cur.prev
			}
			tmp := cur.prev
			half.pages -= cur.release(a.pc)
			cur = tmp
			continue
		}

		// end > cur.end && start < cur.end: drop cur's end (2)
		if diffEnd < 0 && start.Difference(curEnd) > 0 {
			if *debugLog {
				log.Printf("case 2\n")
			}
			cur.bytes = cur.bytes[:-start.Difference(cur.seq)]
			break
		} else

		// start < cur.start && end > cur.start: drop cur's start (4)
		if diffStart > 0 && end.Difference(cur.seq) < 0 {
			if *debugLog {
				log.Printf("case 4\n")
			}
			cur.bytes = cur.bytes[-end.Difference(cur.seq):]
			cur.seq = cur.seq.Add(-end.Difference(cur.seq))
			next = cur
		} else

		// end < cur.end && start > cur.start: replace bytes inside cur (6)
		if diffEnd > 0 && diffStart < 0 {
			if *debugLog {
				log.Printf("case 6\n")
			}
			copy(cur.bytes[-diffStart:-diffStart+len(bytes)], bytes)
			bytes = bytes[:0]
		} else {
			if *debugLog {
				log.Printf("no overlap\n")
			}
			next = cur
		}
		cur = cur.prev
	}

	// Split bytes into pages, and insert in queue
	a.cacheLP.bytes = bytes
	a.cacheLP.seq = start
	if len(bytes) > 0 && queue {
		p, p2, numPages := a.cacheLP.convertToPages(a.pc, 0, ac)
		half.queuedPackets++
		half.queuedBytes += len(bytes)
		half.pages += numPages
		if cur != nil {
			if *debugLog {
				log.Printf("adding %s after %s", p, cur)
			}
			cur.next = p
			p.prev = cur
		} else {
			if *debugLog {
				log.Printf("adding %s as first", p)
			}
			half.first = p
		}
		if next != nil {
			if *debugLog {
				log.Printf("setting %s as next of new %s", next, p2)
			}
			p2.next = next
			next.prev = p2
		} else {
			if *debugLog {
				log.Printf("setting %s as last", p2)
			}
			half.last = p2
		}
	}
	a.dump("After checkOverlap", half)
}

// Warning: this is a low-level dumper, i.e. a.ret or a.cacheSG might
// be strange, but it could be ok.
func (a *Assembler) dump(text string, half *halfconnection) {
	if !*debugLog {
		return
	}
	log.Printf("%s: dump\n", text)
	if half != nil {
		p := half.first
		if p == nil {
			log.Printf(" * half.first = %p, no chunks queued\n", p)
		} else {
			s := 0
			nb := 0
			log.Printf(" * half.first = %p, queued chunks:", p)
			for p != nil {
				log.Printf("\t%s bytes:%s\n", p, hex.EncodeToString(p.bytes))
				s += len(p.bytes)
				nb++
				p = p.next
			}
			log.Printf("\t%d chunks for %d bytes", nb, s)
		}
		log.Printf(" * half.last = %p\n", half.last)
		log.Printf(" * half.saved = %p\n", half.saved)
		p = half.saved
		for p != nil {
			log.Printf("\tseq:%d %s bytes:%s\n", p.getSeq(), p, hex.EncodeToString(p.bytes))
			p = p.next
		}
	}
	log.Printf(" * a.ret\n")
	for i, r := range a.ret {
		log.Printf("\t%d: %v b:%s\n", i, r.captureInfo(), hex.EncodeToString(r.getBytes()))
	}
	log.Printf(" * a.cacheSG.all\n")
	for i, r := range a.cacheSG.all {
		log.Printf("\t%d: %v b:%s\n", i, r.captureInfo(), hex.EncodeToString(r.getBytes()))
	}
}

func (a *Assembler) overlapExisting(half *halfconnection, start, end Sequence, bytes []byte) ([]byte, Sequence) {
	if half.nextSeq == invalidSequence {
		// no start yet
		return bytes, start
	}
	diff := start.Difference(half.nextSeq)
	if diff == 0 {
		return bytes, start
	}
	s := 0
	e := len(bytes)
	// TODO: depending on strategy, we might want to shrink half.saved if possible
	if e != 0 {
		if *debugLog {
			log.Printf("Overlap detected: ignoring current packet's first %d bytes", diff)
		}
		half.overlapPackets++
		half.overlapBytes += diff
	}
	s += diff
	if s >= e {
		// Completely included in sent
		s = e
	}
	bytes = bytes[s:]
	return bytes, half.nextSeq
}

// Prepare send or queue
func (a *Assembler) handleBytes(bytes []byte, seq Sequence, half *halfconnection, ci gopacket.CaptureInfo, start bool, end bool, action assemblerAction, ac AssemblerContext) assemblerAction {
	a.cacheLP.bytes = bytes
	a.cacheLP.start = start
	a.cacheLP.end = end
	a.cacheLP.seq = seq
	a.cacheLP.ci = ci
	a.cacheLP.ac = ac

	if action.queue {
		a.checkOverlap(half, true, ac)
		if (a.MaxBufferedPagesPerConnection > 0 && half.pages >= a.MaxBufferedPagesPerConnection) ||
			(a.MaxBufferedPagesTotal > 0 && a.pc.used >= a.MaxBufferedPagesTotal) {
			if *debugLog {
				log.Printf("hit max buffer size: %+v, %v, %v", a.AssemblerOptions, half.pages, a.pc.used)
			}
			action.queue = false
			a.addNextFromConn(half)
		}
		a.dump("handleBytes after queue", half)
	} else {
		a.cacheLP.bytes, a.cacheLP.seq = a.overlapExisting(half, seq, seq.Add(len(bytes)), a.cacheLP.bytes)
		a.checkOverlap(half, false, ac)
		if len(a.cacheLP.bytes) != 0 || end || start {
			a.ret = append(a.ret, &a.cacheLP)
		}
		a.dump("handleBytes after no queue", half)
	}
	return action
}

func (a *Assembler) setStatsToSG(half *halfconnection) {
	a.cacheSG.queuedBytes = half.queuedBytes
	half.queuedBytes = 0
	a.cacheSG.queuedPackets = half.queuedPackets
	half.queuedPackets = 0
	a.cacheSG.overlapBytes = half.overlapBytes
	half.overlapBytes = 0
	a.cacheSG.overlapPackets = half.overlapPackets
	half.overlapPackets = 0
}

// Build the ScatterGather object, i.e. prepend saved bytes and
// append continuous bytes.
func (a *Assembler) buildSG(half *halfconnection) (bool, Sequence) {
	// find if there are skipped bytes
	skip := -1
	if half.nextSeq != invalidSequence {
		skip = half.nextSeq.Difference(a.ret[0].getSeq())
	}
	last := a.ret[0].getSeq().Add(a.ret[0].length())
	// Prepend saved bytes
	saved := a.addPending(half, a.ret[0].getSeq())
	// Append continuous bytes
	nextSeq := a.addContiguous(half, last)
	a.cacheSG.all = a.ret
	a.cacheSG.Direction = half.dir
	a.cacheSG.Skip = skip
	a.cacheSG.saved = saved
	a.cacheSG.toKeep = -1
	a.setStatsToSG(half)
	a.dump("after buildSG", half)
	return a.ret[len(a.ret)-1].isEnd(), nextSeq
}

func (a *Assembler) cleanSG(half *halfconnection, ac AssemblerContext) {
	cur := 0
	ndx := 0
	skip := 0

	a.dump("cleanSG(start)", half)

	var r byteContainer
	// Find first page to keep
	if a.cacheSG.toKeep < 0 {
		ndx = len(a.cacheSG.all)
	} else {
		skip = a.cacheSG.toKeep
		found := false
		for ndx, r = range a.cacheSG.all {
			if a.cacheSG.toKeep < cur+r.length() {
				found = true
				break
			}
			cur += r.length()
			if skip >= r.length() {
				skip -= r.length()
			}
		}
		if !found {
			ndx++
		}
	}
	// Release consumed pages
	for _, r := range a.cacheSG.all[:ndx] {
		if r == half.saved {
			if half.saved.next != nil {
				half.saved.next.prev = nil
			}
			half.saved = half.saved.next
		} else if r == half.first {
			if half.first.next != nil {
				half.first.next.prev = nil
			}
			if half.first == half.last {
				half.first, half.last = nil, nil
			} else {
				half.first = half.first.next
			}
		}
		half.pages -= r.release(a.pc)
	}
	a.dump("after consumed release", half)
	// Keep un-consumed pages
	nbKept := 0
	half.saved = nil
	var saved *page
	for _, r := range a.cacheSG.all[ndx:] {
		first, last, nb := r.convertToPages(a.pc, skip, ac)
		if half.saved == nil {
			half.saved = first
		} else {
			saved.next = first
			first.prev = saved
		}
		saved = last
		nbKept += nb
	}
	if *debugLog {
		log.Printf("Remaining %d chunks in SG\n", nbKept)
		log.Printf("%s\n", a.Dump())
		a.dump("after cleanSG()", half)
	}
}

// sendToConnection sends the current values in a.ret to the connection, closing
// the connection if the last thing sent had End set.
func (a *Assembler) sendToConnection(conn *connection, half *halfconnection, ac AssemblerContext) Sequence {
	if *debugLog {
		log.Printf("sendToConnection\n")
	}
	end, nextSeq := a.buildSG(half)
	half.stream.ReassembledSG(&a.cacheSG, ac)
	a.cleanSG(half, ac)
	if end {
		a.closeHalfConnection(conn, half)
	}
	if *debugLog {
		log.Printf("after sendToConnection: nextSeq: %d\n", nextSeq)
	}
	return nextSeq
}

//
func (a *Assembler) addPending(half *halfconnection, firstSeq Sequence) int {
	if half.saved == nil {
		return 0
	}
	s := 0
	ret := []byteContainer{}
	for p := half.saved; p != nil; p = p.next {
		if *debugLog {
			log.Printf("adding pending @%p %s (%s)\n", p, p, hex.EncodeToString(p.bytes))
		}
		ret = append(ret, p)
		s += len(p.bytes)
	}
	if half.saved.seq.Add(s) != firstSeq {
		// non-continuous saved: drop them
		var next *page
		for p := half.saved; p != nil; p = next {
			next = p.next
			p.release(a.pc)
		}
		half.saved = nil
		ret = []byteContainer{}
		s = 0
	}

	a.ret = append(ret, a.ret...)
	return s
}

// addContiguous adds contiguous byte-sets to a connection.
func (a *Assembler) addContiguous(half *halfconnection, lastSeq Sequence) Sequence {
	page := half.first
	if page == nil {
		if *debugLog {
			log.Printf("addContiguous(%d): no pages\n", lastSeq)
		}
		return lastSeq
	}
	if lastSeq == invalidSequence {
		lastSeq = page.seq
	}
	for page != nil && lastSeq.Difference(page.seq) == 0 {
		if *debugLog {
			log.Printf("addContiguous: lastSeq: %d, first.seq=%d, page.seq=%d\n", half.nextSeq, half.first.seq, page.seq)
		}
		lastSeq = lastSeq.Add(len(page.bytes))
		a.ret = append(a.ret, page)
		half.first = page.next
		if half.first == nil {
			half.last = nil
		}
		if page.next != nil {
			page.next.prev = nil
		}
		page = page.next
	}
	return lastSeq
}

// skipFlush skips the first set of bytes we're waiting for and returns the
// first set of bytes we have.  If we have no bytes saved, it closes the
// connection.
func (a *Assembler) skipFlush(conn *connection, half *halfconnection) {
	if *debugLog {
		log.Printf("skipFlush %v\n", half.nextSeq)
	}
	// Well, it's embarassing it there is still something in half.saved
	// FIXME: change API to give back saved + new/no packets
	if half.first == nil {
		a.closeHalfConnection(conn, half)
		return
	}
	a.ret = a.ret[:0]
	a.addNextFromConn(half)
	nextSeq := a.sendToConnection(conn, half, a.ret[0].assemblerContext())
	if nextSeq != invalidSequence {
		half.nextSeq = nextSeq
	}
}

func (a *Assembler) closeHalfConnection(conn *connection, half *halfconnection) {
	if *debugLog {
		log.Printf("%v closing", conn)
	}
	half.closed = true
	for p := half.first; p != nil; p = p.next {
		// FIXME: it should be already empty
		a.pc.replace(p)
		half.pages--
	}
	if conn.s2c.closed && conn.c2s.closed {
		if half.stream.ReassemblyComplete(nil) { //FIXME: which context to pass ?
			a.connPool.remove(conn)
		}
	}
}

// addNextFromConn pops the first page from a connection off and adds it to the
// return array.
func (a *Assembler) addNextFromConn(conn *halfconnection) {
	if conn.first == nil {
		return
	}
	if *debugLog {
		log.Printf("   adding from conn (%v, %v) %v (%d)\n", conn.first.seq, conn.nextSeq, conn.nextSeq-conn.first.seq, len(conn.first.bytes))
	}
	a.ret = append(a.ret, conn.first)
	conn.first = conn.first.next
	if conn.first != nil {
		conn.first.prev = nil
	} else {
		conn.last = nil
	}
}

// FlushOptions provide options for flushing connections.
type FlushOptions struct {
	T  time.Time // If nonzero, only connections with data older than T are flushed
	TC time.Time // If nonzero, only connections with data older than TC are closed (if no FIN/RST received)
}

// FlushWithOptions finds any streams waiting for packets older than
// the given time T, and pushes through the data they have (IE: tells
// them to stop waiting and skip the data they're waiting for).
//
// It also closes streams older than TC (that can be set to zero, to keep
// long-lived stream alive, but to flush data anyway).
//
// Each Stream maintains a list of zero or more sets of bytes it has received
// out-of-order.  For example, if it has processed up through sequence number
// 10, it might have bytes [15-20), [20-25), [30,50) in its list.  Each set of
// bytes also has the timestamp it was originally viewed.  A flush call will
// look at the smallest subsequent set of bytes, in this case [15-20), and if
// its timestamp is older than the passed-in time, it will push it and all
// contiguous byte-sets out to the Stream's Reassembled function.  In this case,
// it will push [15-20), but also [20-25), since that's contiguous.  It will
// only push [30-50) if its timestamp is also older than the passed-in time,
// otherwise it will wait until the next FlushCloseOlderThan to see if bytes
// [25-30) come in.
//
// Returns the number of connections flushed, and of those, the number closed
// because of the flush.
func (a *Assembler) FlushWithOptions(opt FlushOptions) (flushed, closed int) {
	conns := a.connPool.connections()
	closes := 0
	flushes := 0
	for _, conn := range conns {
		remove := false
		conn.mu.Lock()
		for _, half := range []*halfconnection{&conn.s2c, &conn.c2s} {
			flushed, closed := a.flushClose(conn, half, opt.T, opt.TC)
			if flushed {
				flushes++
			}
			if closed {
				closes++
			}
		}
		if conn.s2c.closed && conn.c2s.closed && conn.s2c.lastSeen.Before(opt.TC) && conn.c2s.lastSeen.Before(opt.TC) {
			remove = true
		}
		conn.mu.Unlock()
		if remove {
			a.connPool.remove(conn)
		}
	}
	return flushes, closes
}

// FlushCloseOlderThan flushes and closes streams older than given time
func (a *Assembler) FlushCloseOlderThan(t time.Time) (flushed, closed int) {
	return a.FlushWithOptions(FlushOptions{T: t, TC: t})
}

func (a *Assembler) flushClose(conn *connection, half *halfconnection, t time.Time, tc time.Time) (bool, bool) {
	flushed, closed := false, false
	if half.closed {
		return flushed, closed
	}
	for half.first != nil && half.first.seen.Before(t) {
		flushed = true
		a.skipFlush(conn, half)
		if half.closed {
			closed = true
		}
	}
	// Close the connection only if both halfs of the connection last seen before tc.
	if !half.closed && half.first == nil && conn.lastSeen().Before(tc) {
		a.closeHalfConnection(conn, half)
		closed = true
	}
	return flushed, closed
}

// FlushAll flushes all remaining data into all remaining connections and closes
// those connections. It returns the total number of connections flushed/closed
// by the call.
func (a *Assembler) FlushAll() (closed int) {
	conns := a.connPool.connections()
	closed = len(conns)
	for _, conn := range conns {
		conn.mu.Lock()
		for _, half := range []*halfconnection{&conn.s2c, &conn.c2s} {
			for !half.closed {
				a.skipFlush(conn, half)
			}
			if !half.closed {
				a.closeHalfConnection(conn, half)
			}
		}
		conn.mu.Unlock()
	}
	return
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
