// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// Package tcpassembly provides TCP stream re-assembly.
//
// The tcpassembly package implements uni-directional TCP reassembly, for use in
// packet-sniffing applications.  The caller reads packets off the wire, then
// presents them to an Assembler in the form of gopacket layers.TCP packets
// (github.com/google/gopacket, github.com/google/gopacket/layers).
//
// The Assembler uses a user-supplied
// StreamFactory to create a user-defined Stream interface, then passes packet
// data in stream order to that object.  A concurrency-safe StreamPool keeps
// track of all current Streams being reassembled, so multiple Assemblers may
// run at once to assemble packets while taking advantage of multiple cores.
package tcpassembly

import (
	"flag"
	"fmt"
	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"log"
	"sync"
	"time"
)

var memLog = flag.Bool("assembly_memuse_log", false, "If true, the github.com/google/gopacket/tcpassembly library will log information regarding its memory use every once in a while.")
var debugLog = flag.Bool("assembly_debug_log", false, "If true, the github.com/google/gopacket/tcpassembly library will log verbose debugging information (at least one line per packet)")

const invalidSequence = -1
const uint32Size = 1 << 32

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
	if s > uint32Size-uint32Size/4 && t < uint32Size/4 {
		t += uint32Size
	} else if t > uint32Size-uint32Size/4 && s < uint32Size/4 {
		s += uint32Size
	}
	return int(t - s)
}

// Add adds an integer to a sequence and returns the resulting sequence.
func (s Sequence) Add(t int) Sequence {
	return (s + Sequence(t)) & (uint32Size - 1)
}

// Reassembly objects are passed by an Assembler into Streams using the
// Reassembled call.  Callers should not need to create these structs themselves
// except for testing.
type Reassembly struct {
	// Bytes is the next set of bytes in the stream.  May be empty.
	Bytes []byte
	// Skip is set to non-zero if bytes were skipped between this and the
	// last Reassembly.  If this is the first packet in a connection and we
	// didn't see the start, we have no idea how many bytes we skipped, so
	// we set it to -1.  Otherwise, it's set to the number of bytes skipped.
	Skip int
	// Start is set if this set of bytes has a TCP SYN accompanying it.
	Start bool
	// End is set if this set of bytes has a TCP FIN or RST accompanying it.
	End bool
	// Seen is the timestamp this set of bytes was pulled off the wire.
	Seen time.Time
}

const pageBytes = 1900

// page is used to store TCP data we're not ready for yet (out-of-order
// packets).  Unused pages are stored in and returned from a pageCache, which
// avoids memory allocation.  Used pages are stored in a doubly-linked list in
// a connection.
type page struct {
	Reassembly
	seq        Sequence
	index      int
	prev, next *page
	buf        [pageBytes]byte
}

// pageCache is a concurrency-unsafe store of page objects we use to avoid
// memory allocation as much as we can.  It grows but never shrinks.
type pageCache struct {
	free         []*page
	pcSize       int
	size, used   int
	pages        [][]page
	pageRequests int64
}

const initialAllocSize = 1024

func newPageCache() *pageCache {
	pc := &pageCache{
		free:   make([]*page, 0, initialAllocSize),
		pcSize: initialAllocSize,
	}
	pc.grow()
	return pc
}

// grow exponentially increases the size of our page cache as much as necessary.
func (c *pageCache) grow() {
	pages := make([]page, c.pcSize)
	c.pages = append(c.pages, pages)
	c.size += c.pcSize
	for i := range pages {
		c.free = append(c.free, &pages[i])
	}
	if *memLog {
		log.Println("PageCache: created", c.pcSize, "new pages")
	}
	c.pcSize *= 2
}

// next returns a clean, ready-to-use page object.
func (c *pageCache) next(ts time.Time) (p *page) {
	if *memLog {
		c.pageRequests++
		if c.pageRequests&0xFFFF == 0 {
			log.Println("PageCache:", c.pageRequests, "requested,", c.used, "used,", len(c.free), "free")
		}
	}
	if len(c.free) == 0 {
		c.grow()
	}
	i := len(c.free) - 1
	p, c.free = c.free[i], c.free[:i]
	p.prev = nil
	p.next = nil
	p.Reassembly = Reassembly{Bytes: p.buf[:0], Seen: ts}
	c.used++
	return p
}

// replace replaces a page into the pageCache.
func (c *pageCache) replace(p *page) {
	c.used--
	c.free = append(c.free, p)
}

// Stream is implemented by the caller to handle incoming reassembled
// TCP data.  Callers create a StreamFactory, then StreamPool uses
// it to create a new Stream for every TCP stream.
//
// assembly will, in order:
//    1) Create the stream via StreamFactory.New
//    2) Call Reassembled 0 or more times, passing in reassembled TCP data in order
//    3) Call ReassemblyComplete one time, after which the stream is dereferenced by assembly.
type Stream interface {
	// Reassembled is called zero or more times.  assembly guarantees
	// that the set of all Reassembly objects passed in during all
	// calls are presented in the order they appear in the TCP stream.
	// Reassembly objects are reused after each Reassembled call,
	// so it's important to copy anything you need out of them
	// (specifically out of Reassembly.Bytes) that you need to stay
	// around after you return from the Reassembled call.
	Reassembled([]Reassembly)
	// ReassemblyComplete is called when assembly decides there is
	// no more data for this Stream, either because a FIN or RST packet
	// was seen, or because the stream has timed out without any new
	// packet data (due to a call to FlushOlderThan).
	ReassemblyComplete()
}

// StreamFactory is used by assembly to create a new stream for each
// new TCP session.
type StreamFactory interface {
	// New should return a new stream for the given TCP key.
	New(netFlow, tcpFlow gopacket.Flow) Stream
}

func (p *StreamPool) connections() []*connection {
	p.mu.RLock()
	conns := make([]*connection, 0, len(p.conns))
	for _, conn := range p.conns {
		conns = append(conns, conn)
	}
	p.mu.RUnlock()
	return conns
}

// FlushOptions provide options for flushing connections.
type FlushOptions struct {
	T        time.Time // If nonzero, only connections with data older than T are flushed
	CloseAll bool      // If true, ALL connections are closed post flush, not just those that correctly see FIN/RST.
}

// FlushWithOptions finds any streams waiting for packets older than
// the given time, and pushes through the data they have (IE: tells
// them to stop waiting and skip the data they're waiting for).
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
// otherwise it will wait until the next FlushOlderThan to see if bytes [25-30)
// come in.
//
// If it pushes all bytes (or there were no sets of bytes to begin with)
// AND the connection has not received any bytes since the passed-in time,
// the connection will be closed.
//
// If CloseAll is set, it will close out connections that have been drained.
// Regardless of the CloseAll setting, connections stale for the specified
// time will be closed.
//
// Returns the number of connections flushed, and of those, the number closed
// because of the flush.
func (a *Assembler) FlushWithOptions(opt FlushOptions) (flushed, closed int) {
	conns := a.connPool.connections()
	closes := 0
	flushes := 0
	for _, conn := range conns {
		flushed := false
		conn.mu.Lock()
		if conn.closed {
			// Already closed connection, nothing to do here.
			conn.mu.Unlock()
			continue
		}
		for conn.first != nil && conn.first.Seen.Before(opt.T) {
			a.skipFlush(conn)
			flushed = true
			if conn.closed {
				closes++
				break
			}
		}
		if opt.CloseAll && !conn.closed && conn.first == nil && conn.lastSeen.Before(opt.T) {
			flushed = true
			a.closeConnection(conn)
			closes++
		}
		if flushed {
			flushes++
		}
		conn.mu.Unlock()
	}
	return flushes, closes
}

// FlushOlderThan calls FlushWithOptions with the CloseAll option set to true.
func (a *Assembler) FlushOlderThan(t time.Time) (flushed, closed int) {
	return a.FlushWithOptions(FlushOptions{CloseAll: true, T: t})
}

// FlushAll flushes all remaining data into all remaining connections, closing
// those connections.  It returns the total number of connections flushed/closed
// by the call.
func (a *Assembler) FlushAll() (closed int) {
	conns := a.connPool.connections()
	closed = len(conns)
	for _, conn := range conns {
		conn.mu.Lock()
		for !conn.closed {
			a.skipFlush(conn)
		}
		conn.mu.Unlock()
	}
	return
}

type key [2]gopacket.Flow

func (k *key) String() string {
	return fmt.Sprintf("%s:%s", k[0], k[1])
}

// StreamPool stores all streams created by Assemblers, allowing multiple
// assemblers to work together on stream processing while enforcing the fact
// that a single stream receives its data serially.  It is safe
// for concurrency, usable by multiple Assemblers at once.
//
// StreamPool handles the creation and storage of Stream objects used by one or
// more Assembler objects.  When a new TCP stream is found by an Assembler, it
// creates an associated Stream by calling its StreamFactory's New method.
// Thereafter (until the stream is closed), that Stream object will receive
// assembled TCP data via Assembler's calls to the stream's Reassembled
// function.
//
// Like the Assembler, StreamPool attempts to minimize allocation.  Unlike the
// Assembler, though, it does have to do some locking to make sure that the
// connection objects it stores are accessible to multiple Assemblers.
type StreamPool struct {
	conns              map[key]*connection
	users              int
	mu                 sync.RWMutex
	factory            StreamFactory
	free               []*connection
	all                [][]connection
	nextAlloc          int
	newConnectionCount int64
}

func (p *StreamPool) grow() {
	conns := make([]connection, p.nextAlloc)
	p.all = append(p.all, conns)
	for i := range conns {
		p.free = append(p.free, &conns[i])
	}
	if *memLog {
		log.Println("StreamPool: created", p.nextAlloc, "new connections")
	}
	p.nextAlloc *= 2
}

// NewStreamPool creates a new connection pool.  Streams will
// be created as necessary using the passed-in StreamFactory.
func NewStreamPool(factory StreamFactory) *StreamPool {
	return &StreamPool{
		conns:     make(map[key]*connection, initialAllocSize),
		free:      make([]*connection, 0, initialAllocSize),
		factory:   factory,
		nextAlloc: initialAllocSize,
	}
}

const assemblerReturnValueInitialSize = 16

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
		ret:              make([]Reassembly, assemblerReturnValueInitialSize),
		pc:               newPageCache(),
		connPool:         pool,
		AssemblerOptions: DefaultAssemblerOptions,
	}
}

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

type connection struct {
	key               key
	pages             int
	first, last       *page
	nextSeq           Sequence
	created, lastSeen time.Time
	stream            Stream
	closed            bool
	mu                sync.Mutex
}

func (c *connection) reset(k key, s Stream, ts time.Time) {
	c.key = k
	c.pages = 0
	c.first, c.last = nil, nil
	c.nextSeq = invalidSequence
	c.created = ts
	c.stream = s
	c.closed = false
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
	ret      []Reassembly
	pc       *pageCache
	connPool *StreamPool
}

func (p *StreamPool) newConnection(k key, s Stream, ts time.Time) (c *connection) {
	if *memLog {
		p.newConnectionCount++
		if p.newConnectionCount&0x7FFF == 0 {
			log.Println("StreamPool:", p.newConnectionCount, "requests,", len(p.conns), "used,", len(p.free), "free")
		}
	}
	if len(p.free) == 0 {
		p.grow()
	}
	index := len(p.free) - 1
	c, p.free = p.free[index], p.free[:index]
	c.reset(k, s, ts)
	return c
}

// getConnection returns a connection.  If end is true and a connection
// does not already exist, returns nil.  This allows us to check for a
// connection without actually creating one if it doesn't already exist.
func (p *StreamPool) getConnection(k key, end bool, ts time.Time) *connection {
	p.mu.RLock()
	conn := p.conns[k]
	p.mu.RUnlock()
	if end || conn != nil {
		return conn
	}
	s := p.factory.New(k[0], k[1])
	p.mu.Lock()
	conn = p.newConnection(k, s, ts)
	if conn2 := p.conns[k]; conn2 != nil {
		p.mu.Unlock()
		return conn2
	}
	p.conns[k] = conn
	p.mu.Unlock()
	return conn
}

// Assemble calls AssembleWithTimestamp with the current timestamp, useful for
// packets being read directly off the wire.
func (a *Assembler) Assemble(netFlow gopacket.Flow, t *layers.TCP) {
	a.AssembleWithTimestamp(netFlow, t, time.Now())
}

// AssembleWithTimestamp reassembles the given TCP packet into its appropriate
// stream.
//
// The timestamp passed in must be the timestamp the packet was seen.
// For packets read off the wire, time.Now() should be fine.  For packets read
// from PCAP files, CaptureInfo.Timestamp should be passed in.  This timestamp
// will affect which streams are flushed by a call to FlushOlderThan.
//
// Each Assemble call results in, in order:
//
//    zero or one calls to StreamFactory.New, creating a stream
//    zero or one calls to Reassembled on a single stream
//    zero or one calls to ReassemblyComplete on the same stream
func (a *Assembler) AssembleWithTimestamp(netFlow gopacket.Flow, t *layers.TCP, timestamp time.Time) {
	// Ignore empty TCP packets
	if !t.SYN && !t.FIN && !t.RST && len(t.LayerPayload()) == 0 {
		if *debugLog {
			log.Println("ignoring useless packet")
		}
		return
	}

	a.ret = a.ret[:0]
	key := key{netFlow, t.TransportFlow()}
	var conn *connection
	// This for loop handles a race condition where a connection will close, lock
	// the connection pool, and remove itself, but before it locked the connection
	// pool it's returned to another Assemble statement.  This should loop 0-1
	// times for the VAST majority of cases.
	for {
		conn = a.connPool.getConnection(
			key, !t.SYN && len(t.LayerPayload()) == 0, timestamp)
		if conn == nil {
			if *debugLog {
				log.Printf("%v got empty packet on otherwise empty connection", key)
			}
			return
		}
		conn.mu.Lock()
		if !conn.closed {
			break
		}
		conn.mu.Unlock()
	}
	if conn.lastSeen.Before(timestamp) {
		conn.lastSeen = timestamp
	}
	seq, bytes := Sequence(t.Seq), t.Payload
	if conn.nextSeq == invalidSequence {
		if t.SYN {
			if *debugLog {
				log.Printf("%v saw first SYN packet, returning immediately, seq=%v", key, seq)
			}
			a.ret = append(a.ret, Reassembly{
				Bytes: bytes,
				Skip:  0,
				Start: true,
				Seen:  timestamp,
			})
			conn.nextSeq = seq.Add(len(bytes) + 1)
		} else {
			if *debugLog {
				log.Printf("%v waiting for start, storing into connection", key)
			}
			a.insertIntoConn(t, conn, timestamp)
		}
	} else if diff := conn.nextSeq.Difference(seq); diff > 0 {
		if *debugLog {
			log.Printf("%v gap in sequence numbers (%v, %v) diff %v, storing into connection", key, conn.nextSeq, seq, diff)
		}
		a.insertIntoConn(t, conn, timestamp)
	} else {
		bytes, conn.nextSeq = byteSpan(conn.nextSeq, seq, bytes)
		if *debugLog {
			log.Printf("%v found contiguous data (%v, %v), returning immediately", key, seq, conn.nextSeq)
		}
		a.ret = append(a.ret, Reassembly{
			Bytes: bytes,
			Skip:  0,
			End:   t.RST || t.FIN,
			Seen:  timestamp,
		})
	}
	if len(a.ret) > 0 {
		a.sendToConnection(conn)
	}
	conn.mu.Unlock()
}

func byteSpan(expected, received Sequence, bytes []byte) (toSend []byte, next Sequence) {
	if expected == invalidSequence {
		return bytes, received.Add(len(bytes))
	}
	span := int(received.Difference(expected))
	if span <= 0 {
		return bytes, received.Add(len(bytes))
	} else if len(bytes) < span {
		return nil, expected
	}
	return bytes[span:], expected.Add(len(bytes) - span)
}

// sendToConnection sends the current values in a.ret to the connection, closing
// the connection if the last thing sent had End set.
func (a *Assembler) sendToConnection(conn *connection) {
	a.addContiguous(conn)
	if conn.stream == nil {
		panic("why?")
	}
	conn.stream.Reassembled(a.ret)
	if a.ret[len(a.ret)-1].End {
		a.closeConnection(conn)
	}
}

// addContiguous adds contiguous byte-sets to a connection.
func (a *Assembler) addContiguous(conn *connection) {
	for conn.first != nil && conn.nextSeq.Difference(conn.first.seq) <= 0 {
		a.addNextFromConn(conn)
	}
}

// skipFlush skips the first set of bytes we're waiting for and returns the
// first set of bytes we have.  If we have no bytes pending, it closes the
// connection.
func (a *Assembler) skipFlush(conn *connection) {
	if *debugLog {
		log.Printf("%v skipFlush %v", conn.key, conn.nextSeq)
	}
	if conn.first == nil {
		a.closeConnection(conn)
		return
	}
	a.ret = a.ret[:0]
	a.addNextFromConn(conn)
	a.addContiguous(conn)
	a.sendToConnection(conn)
}

func (p *StreamPool) remove(conn *connection) {
	p.mu.Lock()
	delete(p.conns, conn.key)
	p.free = append(p.free, conn)
	p.mu.Unlock()
}

func (a *Assembler) closeConnection(conn *connection) {
	if *debugLog {
		log.Printf("%v closing", conn.key)
	}
	conn.stream.ReassemblyComplete()
	conn.closed = true
	a.connPool.remove(conn)
	for p := conn.first; p != nil; p = p.next {
		a.pc.replace(p)
	}
}

// traverseConn traverses our doubly-linked list of pages for the correct
// position to put the given sequence number.  Note that it traverses backwards,
// starting at the highest sequence number and going down, since we assume the
// common case is that TCP packets for a stream will appear in-order, with
// minimal loss or packet reordering.
func (c *connection) traverseConn(seq Sequence) (prev, current *page) {
	prev = c.last
	for prev != nil && prev.seq.Difference(seq) < 0 {
		current = prev
		prev = current.prev
	}
	return
}

// pushBetween inserts the doubly-linked list first-...-last in between the
// nodes prev-next in another doubly-linked list.  If prev is nil, makes first
// the new first page in the connection's list.  If next is nil, makes last the
// new last page in the list.  first/last may point to the same page.
func (c *connection) pushBetween(prev, next, first, last *page) {
	// Maintain our doubly linked list
	if next == nil || c.last == nil {
		c.last = last
	} else {
		last.next = next
		next.prev = last
	}
	if prev == nil || c.first == nil {
		c.first = first
	} else {
		first.prev = prev
		prev.next = first
	}
}

func (a *Assembler) insertIntoConn(t *layers.TCP, conn *connection, ts time.Time) {
	if conn.first != nil && conn.first.seq == conn.nextSeq {
		panic("wtf")
	}
	p, p2, numPages := a.pagesFromTCP(t, ts)
	prev, current := conn.traverseConn(Sequence(t.Seq))
	conn.pushBetween(prev, current, p, p2)
	conn.pages += numPages
	if (a.MaxBufferedPagesPerConnection > 0 && conn.pages >= a.MaxBufferedPagesPerConnection) ||
		(a.MaxBufferedPagesTotal > 0 && a.pc.used >= a.MaxBufferedPagesTotal) {
		if *debugLog {
			log.Printf("%v hit max buffer size: %+v, %v, %v", conn.key, a.AssemblerOptions, conn.pages, a.pc.used)
		}
		a.addNextFromConn(conn)
	}
}

// pagesFromTCP creates a page (or set of pages) from a TCP packet.  Note that
// it should NEVER receive a SYN packet, as it doesn't handle sequences
// correctly.
//
// It returns the first and last page in its doubly-linked list of new pages.
func (a *Assembler) pagesFromTCP(t *layers.TCP, ts time.Time) (p, p2 *page, numPages int) {
	first := a.pc.next(ts)
	current := first
	numPages++
	seq, bytes := Sequence(t.Seq), t.Payload
	for {
		length := min(len(bytes), pageBytes)
		current.Bytes = current.buf[:length]
		copy(current.Bytes, bytes)
		current.seq = seq
		bytes = bytes[length:]
		if len(bytes) == 0 {
			break
		}
		seq = seq.Add(length)
		current.next = a.pc.next(ts)
		current.next.prev = current
		current = current.next
		numPages++
	}
	current.End = t.RST || t.FIN
	return first, current, numPages
}

// addNextFromConn pops the first page from a connection off and adds it to the
// return array.
func (a *Assembler) addNextFromConn(conn *connection) {
	if conn.nextSeq == invalidSequence {
		conn.first.Skip = -1
	} else if diff := conn.nextSeq.Difference(conn.first.seq); diff > 0 {
		conn.first.Skip = int(diff)
	}
	conn.first.Bytes, conn.nextSeq = byteSpan(conn.nextSeq, conn.first.seq, conn.first.Bytes)
	if *debugLog {
		log.Printf("%v   adding from conn (%v, %v)", conn.key, conn.first.seq, conn.nextSeq)
	}
	a.ret = append(a.ret, conn.first.Reassembly)
	a.pc.replace(conn.first)
	if conn.first == conn.last {
		conn.first = nil
		conn.last = nil
	} else {
		conn.first = conn.first.next
		conn.first.prev = nil
	}
	conn.pages--
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
