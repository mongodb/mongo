// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build linux

// Package afpacket provides Go bindings for MMap'd AF_PACKET socket reading.
package afpacket

// Couldn't have done this without:
// http://lxr.free-electrons.com/source/Documentation/networking/packet_mmap.txt
// http://codemonkeytips.blogspot.co.uk/2011/07/asynchronous-packet-socket-reading-with.html

import (
	"errors"
	"fmt"
	"github.com/google/gopacket"
	"net"
	"runtime"
	"sync"
	"time"
	"unsafe"
)

/*
#include <linux/if_packet.h>  // AF_PACKET, sockaddr_ll
#include <linux/if_ether.h>  // ETH_P_ALL
#include <sys/socket.h>  // socket()
#include <unistd.h>  // close()
#include <arpa/inet.h>  // htons()
#include <sys/mman.h>  // mmap(), munmap()
#include <poll.h>  // poll()
*/
import "C"

var pageSize = int(C.getpagesize())
var tpacketAlignment = uint(C.TPACKET_ALIGNMENT)

func tpacketAlign(v int) int {
	return int((uint(v) + tpacketAlignment - 1) & ((^tpacketAlignment) - 1))
}

// Stats is a set of counters detailing the work TPacket has done so far.
type Stats struct {
	// Packets is the total number of packets returned to the caller.
	Packets int64
	// Polls is the number of blocking syscalls made waiting for packets.
	// This should always be <= Packets, since with TPacket one syscall
	// can (and often does) return many results.
	Polls int64
}

type TPacket struct {
	// fd is the C file descriptor.
	fd C.int
	// ring points to the memory space of the ring buffer shared by tpacket and the kernel.
	ring unsafe.Pointer
	// opts contains read-only options for the TPacket object.
	opts options
	mu   sync.Mutex // guards below
	// offset is the offset into the ring of the current header.
	offset int
	// current is the current header.
	current header
	// pollset is used by TPacket for its poll() call.
	pollset C.struct_pollfd
	// shouldReleasePacket is set to true whenever we return packet data, to make sure we remember to release that data back to the kernel.
	shouldReleasePacket bool
	// stats is simple statistics on TPacket's run.
	stats Stats
	// tpVersion is the version of TPacket actually in use, set by setRequestedTPacketVersion.
	tpVersion OptTPacketVersion
	// Hackity hack hack hack.  We need to return a pointer to the header with
	// getTPacketHeader, and we don't want to allocate a v3wrapper every time,
	// so we leave it in the TPacket object and return a pointer to it.
	v3 v3wrapper
}

// bindToInterface binds the TPacket socket to a particular named interface.
func (h *TPacket) bindToInterface(ifaceName string) error {
	iface, err := net.InterfaceByName(ifaceName)
	if err != nil {
		return fmt.Errorf("InterfaceByName: %v", err)
	}
	var ll C.struct_sockaddr_ll
	ll.sll_family = C.AF_PACKET
	ll.sll_protocol = C.__be16(C.htons(C.ETH_P_ALL))
	ll.sll_ifindex = C.int(iface.Index)
	if _, err := C.bind(h.fd, (*C.struct_sockaddr)(unsafe.Pointer(&ll)), C.socklen_t(unsafe.Sizeof(ll))); err != nil {
		return fmt.Errorf("bindToInterface: %v", err)
	}
	return nil
}

// setTPacketVersion asks the kernel to set TPacket to a particular version, and returns an error on failure.
func (h *TPacket) setTPacketVersion(version OptTPacketVersion) error {
	val := C.int(version)
	_, err := C.setsockopt(h.fd, C.SOL_PACKET, C.PACKET_VERSION, unsafe.Pointer(&val), C.socklen_t(unsafe.Sizeof(val)))
	if err != nil {
		return fmt.Errorf("setsockopt packet_version: %v", err)
	}
	return nil
}

// setRequestedTPacketVersion tries to set TPacket to the requested version or versions.
func (h *TPacket) setRequestedTPacketVersion() error {
	switch {
	case (h.opts.version == TPacketVersionHighestAvailable || h.opts.version == TPacketVersion3) && h.setTPacketVersion(TPacketVersion3) == nil:
		h.tpVersion = TPacketVersion3
	case (h.opts.version == TPacketVersionHighestAvailable || h.opts.version == TPacketVersion2) && h.setTPacketVersion(TPacketVersion2) == nil:
		h.tpVersion = TPacketVersion2
	case (h.opts.version == TPacketVersionHighestAvailable || h.opts.version == TPacketVersion1) && h.setTPacketVersion(TPacketVersion1) == nil:
		h.tpVersion = TPacketVersion1
	default:
		return errors.New("no known tpacket versions work on this machine")
	}
	return nil
}

// setUpRing sets up the shared-memory ring buffer between the user process and the kernel.
func (h *TPacket) setUpRing() (err error) {
	totalSize := C.uint(h.opts.framesPerBlock * h.opts.numBlocks * h.opts.frameSize)
	switch h.tpVersion {
	case TPacketVersion1, TPacketVersion2:
		var tp C.struct_tpacket_req
		tp.tp_block_size = C.uint(h.opts.blockSize)
		tp.tp_block_nr = C.uint(h.opts.numBlocks)
		tp.tp_frame_size = C.uint(h.opts.frameSize)
		tp.tp_frame_nr = C.uint(h.opts.framesPerBlock * h.opts.numBlocks)
		if _, err := C.setsockopt(h.fd, C.SOL_PACKET, C.PACKET_RX_RING, unsafe.Pointer(&tp), C.socklen_t(unsafe.Sizeof(tp))); err != nil {
			return fmt.Errorf("setsockopt packet_rx_ring: %v", err)
		}
	case TPacketVersion3:
		var tp C.struct_tpacket_req3
		tp.tp_block_size = C.uint(h.opts.blockSize)
		tp.tp_block_nr = C.uint(h.opts.numBlocks)
		tp.tp_frame_size = C.uint(h.opts.frameSize)
		tp.tp_frame_nr = C.uint(h.opts.framesPerBlock * h.opts.numBlocks)
		tp.tp_retire_blk_tov = C.uint(h.opts.blockTimeout / time.Millisecond)
		if _, err := C.setsockopt(h.fd, C.SOL_PACKET, C.PACKET_RX_RING, unsafe.Pointer(&tp), C.socklen_t(unsafe.Sizeof(tp))); err != nil {
			return fmt.Errorf("setsockopt packet_rx_ring v3: %v", err)
		}
	default:
		return errors.New("invalid tpVersion")
	}
	if h.ring, err = C.mmap(nil, C.size_t(totalSize), C.PROT_READ|C.PROT_WRITE, C.MAP_SHARED, C.int(h.fd), 0); err != nil {
		return
	}
	if h.ring == nil {
		return errors.New("no ring")
	}
	return nil
}

// Close cleans up the TPacket.  It should not be used after the Close call.
func (h *TPacket) Close() {
	if h.fd == -1 {
		return // already closed.
	}
	if h.ring != nil {
		C.munmap(h.ring, C.size_t(h.opts.blockSize*h.opts.numBlocks))
	}
	h.ring = nil
	C.close(h.fd)
	h.fd = -1
	runtime.SetFinalizer(h, nil)
}

// NewTPacket returns a new TPacket object for reading packets off the wire.
// Its behavior may be modified by passing in any/all of afpacket.Opt* to this
// function.
// If this function succeeds, the user should be sure to Close the returned
// TPacket when finished with it.
func NewTPacket(opts ...interface{}) (h *TPacket, err error) {
	h = &TPacket{}
	if h.opts, err = parseOptions(opts...); err != nil {
		return nil, err
	}
	fd, err := C.socket(C.AF_PACKET, C.int(h.opts.socktype), C.int(C.htons(C.ETH_P_ALL)))
	if err != nil {
		return nil, err
	}
	h.fd = fd
	if h.opts.iface != "" {
		if err = h.bindToInterface(h.opts.iface); err != nil {
			goto errlbl
		}
	}
	if err = h.setRequestedTPacketVersion(); err != nil {
		goto errlbl
	}
	if err = h.setUpRing(); err != nil {
		goto errlbl
	}
	runtime.SetFinalizer(h, (*TPacket).Close)
	return h, nil
errlbl:
	h.Close()
	return nil, err
}

func (h *TPacket) releaseCurrentPacket() error {
	h.current.clearStatus()
	h.offset++
	h.shouldReleasePacket = false
	return nil
}

// ZeroCopyReadPacketData reads the next packet off the wire, and returns its data.
// The slice returned by ZeroCopyReadPacketData points to bytes owned by the
// TPacket.  Each call to ZeroCopyReadPacketData invalidates any data previously
// returned by ZeroCopyReadPacketData.  Care must be taken not to keep pointers
// to old bytes when using ZeroCopyReadPacketData... if you need to keep data past
// the next time you call ZeroCopyReadPacketData, use ReadPacketDataData, which copies
// the bytes into a new buffer for you.
//  tp, _ := NewTPacket(...)
//  data1, _, _ := tp.ZeroCopyReadPacketData()
//  // do everything you want with data1 here, copying bytes out of it if you'd like to keep them around.
//  data2, _, _ := tp.ZeroCopyReadPacketData()  // invalidates bytes in data1
func (h *TPacket) ZeroCopyReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	h.mu.Lock()
	if h.current == nil || !h.current.next() {
		if h.shouldReleasePacket {
			h.releaseCurrentPacket()
		}
		h.current = h.getTPacketHeader()
		if err = h.pollForFirstPacket(h.current); err != nil {
			h.mu.Unlock()
			return
		}
	}
	data = h.current.getData()
	ci.Timestamp = h.current.getTime()
	ci.CaptureLength = len(data)
	ci.Length = h.current.getLength()
	h.stats.Packets++
	h.mu.Unlock()
	return
}

// Stats returns statistics on the packets the TPacket has seen so far.
func (h *TPacket) Stats() (Stats, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.stats, nil
}

// ReadPacketDataTo reads packet data into a user-supplied buffer.
// This function reads up to the length of the passed-in slice.
// The number of bytes read into data will be returned in ci.CaptureLength,
// which is the minimum of the size of the passed-in buffer and the size of
// the captured packet.
func (h *TPacket) ReadPacketDataTo(data []byte) (ci gopacket.CaptureInfo, err error) {
	var d []byte
	d, ci, err = h.ZeroCopyReadPacketData()
	if err != nil {
		return
	}
	ci.CaptureLength = copy(data, d)
	return
}

// ReadPacketData reads the next packet, copies it into a new buffer, and returns
// that buffer.  Since the buffer is allocated by ReadPacketData, it is safe for long-term
// use.  This implements gopacket.PacketDataSource.
func (h *TPacket) ReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	var d []byte
	d, ci, err = h.ZeroCopyReadPacketData()
	if err != nil {
		return
	}
	data = make([]byte, len(d))
	copy(data, d)
	return
}

func (h *TPacket) getTPacketHeader() header {
	switch h.tpVersion {
	case TPacketVersion1:
		if h.offset >= h.opts.framesPerBlock*h.opts.numBlocks {
			h.offset = 0
		}
		position := uintptr(h.ring) + uintptr(h.opts.frameSize*h.offset)
		return (*v1header)(unsafe.Pointer(position))
	case TPacketVersion2:
		if h.offset >= h.opts.framesPerBlock*h.opts.numBlocks {
			h.offset = 0
		}
		position := uintptr(h.ring) + uintptr(h.opts.frameSize*h.offset)
		return (*v2header)(unsafe.Pointer(position))
	case TPacketVersion3:
		// TPacket3 uses each block to return values, instead of each frame.  Hence we need to rotate when we hit #blocks, not #frames.
		if h.offset >= h.opts.numBlocks {
			h.offset = 0
		}
		position := uintptr(h.ring) + uintptr(h.opts.frameSize*h.offset*h.opts.framesPerBlock)
		h.v3 = initV3Wrapper(unsafe.Pointer(position))
		return &h.v3
	}
	panic("handle tpacket version is invalid")
}

func (h *TPacket) pollForFirstPacket(hdr header) error {
	for hdr.getStatus()&C.TP_STATUS_USER == 0 {
		h.pollset.fd = h.fd
		h.pollset.events = C.POLLIN
		h.pollset.revents = 0
		_, err := C.poll(&h.pollset, 1, -1)
		h.stats.Polls++
		if err != nil {
			return err
		}
	}
	h.shouldReleasePacket = true
	return nil
}

// FanoutType determines the type of fanout to use with a TPacket SetFanout call.
type FanoutType int

const (
	FanoutHash FanoutType = 0
	// It appears that defrag only works with FanoutHash, see:
	// http://lxr.free-electrons.com/source/net/packet/af_packet.c#L1204
	FanoutHashWithDefrag FanoutType = 0x8000
	FanoutLoadBalance    FanoutType = 1
	FanoutCPU            FanoutType = 2
)

// SetFanout activates TPacket's fanout ability.
// Use of Fanout requires creating multiple TPacket objects and the same id/type to
// a SetFanout call on each.  Note that this can be done cross-process, so if two
// different processes both call SetFanout with the same type/id, they'll share
// packets between them.  The same should work for multiple TPacket objects within
// the same process.
func (h *TPacket) SetFanout(t FanoutType, id uint16) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	arg := C.int(t) << 16
	arg |= C.int(id)
	_, err := C.setsockopt(h.fd, C.SOL_PACKET, C.PACKET_FANOUT, unsafe.Pointer(&arg), C.socklen_t(unsafe.Sizeof(arg)))
	return err
}

// WritePacketData transmits a raw packet.
func (h *TPacket) WritePacketData(pkt []byte) error {
	_, err := C.write(h.fd, unsafe.Pointer(&pkt[0]), C.size_t(len(pkt)))
	return err
}
