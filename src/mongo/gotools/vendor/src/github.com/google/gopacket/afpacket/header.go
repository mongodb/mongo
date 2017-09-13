// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build linux

package afpacket

import (
	"reflect"
	"time"
	"unsafe"
)

// #include <linux/if_packet.h>
import "C"

// Our model of handling all TPacket versions is a little hacky, to say the
// least.  We use the header interface to handle interactions with the
// tpacket1/tpacket2 packet header AND the tpacket3 block header.  The big
// difference is that tpacket3's block header implements the next() call to get
// the next packet within the block, while v1/v2 just always return false.

type header interface {
	// getStatus returns the TPacket status of the current header.
	getStatus() int
	// clearStatus clears the status of the current header, releasing its
	// underlying data back to the kernel for future use with new packets.
	// Using the header after calling clearStatus is an error.  clearStatus
	// should only be called after next() returns false.
	clearStatus()
	// getTime returns the timestamp for the current packet pointed to by
	// the header.
	getTime() time.Time
	// getData returns the packet data pointed to by the current header.
	getData() []byte
	// getLength returns the total length of the packet.
	getLength() int
	// next moves this header to point to the next packet it contains,
	// returning true on success (in which case getTime and getData will
	// return values for the new packet) or false if there are no more
	// packets (in which case clearStatus should be called).
	next() bool
}

type v1header C.struct_tpacket_hdr
type v2header C.struct_tpacket2_hdr

func makeSlice(start uintptr, length int) (data []byte) {
	slice := (*reflect.SliceHeader)(unsafe.Pointer(&data))
	slice.Data = start
	slice.Len = length
	slice.Cap = length
	return
}

func (h *v1header) getStatus() int {
	return int(h.tp_status)
}
func (h *v1header) clearStatus() {
	h.tp_status = 0
}
func (h *v1header) getTime() time.Time {
	return time.Unix(int64(h.tp_sec), int64(h.tp_usec)*1000)
}
func (h *v1header) getData() []byte {
	return makeSlice(uintptr(unsafe.Pointer(h))+uintptr(h.tp_mac), int(h.tp_snaplen))
}
func (h *v1header) getLength() int {
	return int(h.tp_len)
}
func (h *v1header) next() bool {
	return false
}
func (h *v2header) getStatus() int {
	return int(h.tp_status)
}
func (h *v2header) clearStatus() {
	h.tp_status = 0
}
func (h *v2header) getTime() time.Time {
	return time.Unix(int64(h.tp_sec), int64(h.tp_nsec))
}
func (h *v2header) getData() []byte {
	return makeSlice(uintptr(unsafe.Pointer(h))+uintptr(h.tp_mac), int(h.tp_snaplen))
}
func (h *v2header) getLength() int {
	return int(h.tp_len)
}
func (h *v2header) next() bool {
	return false
}

type v3wrapper struct {
	block    *C.struct_tpacket_block_desc
	blockhdr *C.struct_tpacket_hdr_v1
	packet   *C.struct_tpacket3_hdr
	used     C.__u32
}

func initV3Wrapper(block unsafe.Pointer) (w v3wrapper) {
	w.block = (*C.struct_tpacket_block_desc)(block)
	w.blockhdr = (*C.struct_tpacket_hdr_v1)(unsafe.Pointer(&w.block.hdr[0]))
	w.packet = (*C.struct_tpacket3_hdr)(unsafe.Pointer(uintptr(block) + uintptr(w.blockhdr.offset_to_first_pkt)))
	return
}
func (w *v3wrapper) getStatus() int {
	return int(w.blockhdr.block_status)
}
func (w *v3wrapper) clearStatus() {
	w.blockhdr.block_status = 0
}
func (w *v3wrapper) getTime() time.Time {
	return time.Unix(int64(w.packet.tp_sec), int64(w.packet.tp_nsec))
}
func (w *v3wrapper) getData() []byte {
	return makeSlice(uintptr(unsafe.Pointer(w.packet))+uintptr(w.packet.tp_mac), int(w.packet.tp_snaplen))
}
func (w *v3wrapper) getLength() int {
	return int(w.packet.tp_len)
}
func (w *v3wrapper) next() bool {
	w.used++
	if w.used >= w.blockhdr.num_pkts {
		return false
	}
	next := uintptr(unsafe.Pointer(w.packet))
	if w.packet.tp_next_offset != 0 {
		next += uintptr(w.packet.tp_next_offset)
	} else {
		next += uintptr(tpacketAlign(int(w.packet.tp_snaplen) + int(w.packet.tp_mac)))
	}
	w.packet = (*C.struct_tpacket3_hdr)(unsafe.Pointer(next))
	return true
}
