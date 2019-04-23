// Copyright 2012 Google, Inc. All rights reserved.
// Copyright 2009-2011 Andreas Krennmair. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pfring

/*
// lpcap is needed for bpf
#cgo LDFLAGS: -lpfring -lpcap
#include <stdlib.h>
#include <pfring.h>
#include <stdint.h>
#include <linux/pf_ring.h>

struct metadata {
	u_int64_t timestamp_ns;
	u_int32_t caplen;
	u_int32_t len;
	int32_t if_index;
};

// In pfring 7.2 pfring_pkthdr struct was changed to packed
// Since this is incompatible with go, copy the values we need to a custom
// struct (struct metadata above).
// Another way to do this, would be to store the struct offsets in defines
// and use encoding/binary in go-land. But this has the downside, that there is
// no native endianess in encoding/binary and storing ByteOrder in a variable
// leads to an expensive itab lookup + call (instead of very fast inlined and
// optimized movs). Using unsafe magic could lead to problems with unaligned
// access.
// Additionally, this does the same uintptr-dance as pcap.
int pfring_readpacketdatato_wrapper(
    pfring* ring,
    uintptr_t buffer,
    uintptr_t meta) {
  struct metadata* ci = (struct metadata* )meta;
  struct pfring_pkthdr hdr;
  int ret = pfring_recv(ring, (u_char**)buffer, 0, &hdr, 1);
  ci->timestamp_ns = hdr.extended_hdr.timestamp_ns;
  ci->caplen = hdr.caplen;
  ci->len = hdr.len;
  ci->if_index = hdr.extended_hdr.if_index;
  return ret;
}
*/
import "C"

// NOTE:  If you install PF_RING with non-standard options, you may also need
// to use LDFLAGS -lnuma and/or -lrt.  Both have been reported necessary if
// PF_RING is configured with --disable-bpf.

import (
	"fmt"
	"net"
	"os"
	"reflect"
	"strconv"
	"sync"
	"time"
	"unsafe"

	"github.com/google/gopacket"
)

const errorBufferSize = 256

// Ring provides a handle to a pf_ring.
type Ring struct {
	cptr                    *C.pfring
	useExtendedPacketHeader bool
	interfaceIndex          int
	mu                      sync.Mutex

	meta   C.struct_metadata
	bufPtr *C.u_char
}

// Flag provides a set of boolean flags to use when creating a new ring.
type Flag uint32

// Set of flags that can be passed (OR'd together) to NewRing.
const (
	FlagReentrant       Flag = C.PF_RING_REENTRANT
	FlagLongHeader      Flag = C.PF_RING_LONG_HEADER
	FlagPromisc         Flag = C.PF_RING_PROMISC
	FlagDNASymmetricRSS Flag = C.PF_RING_DNA_SYMMETRIC_RSS
	FlagTimestamp       Flag = C.PF_RING_TIMESTAMP
	FlagHWTimestamp     Flag = C.PF_RING_HW_TIMESTAMP
)

// NewRing creates a new PFRing.  Note that when the ring is initially created,
// it is disabled.  The caller must call Enable to start receiving packets.
// The caller should call Close on the given ring when finished with it.
func NewRing(device string, snaplen uint32, flags Flag) (ring *Ring, _ error) {
	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))

	cptr, err := C.pfring_open(dev, C.u_int32_t(snaplen), C.u_int32_t(flags))
	if cptr == nil || err != nil {
		return nil, fmt.Errorf("pfring NewRing error: %v", err)
	}
	ring = &Ring{cptr: cptr}

	if flags&FlagLongHeader == FlagLongHeader {
		ring.useExtendedPacketHeader = true
	} else {
		ifc, err := net.InterfaceByName(device)
		if err == nil {
			ring.interfaceIndex = ifc.Index
		}
	}
	ring.SetApplicationName(os.Args[0])
	return
}

// Close closes the given Ring.  After this call, the Ring should no longer be
// used.
func (r *Ring) Close() {
	C.pfring_close(r.cptr)
}

// NextResult is the return code from a call to Next.
type NextResult int32

// Set of results that could be returned from a call to get another packet.
const (
	NextNoPacketNonblocking NextResult = 0
	NextError               NextResult = -1
	NextOk                  NextResult = 1
	NextNotEnabled          NextResult = -7
)

// NextResult implements the error interface.
func (n NextResult) Error() string {
	switch n {
	case NextNoPacketNonblocking:
		return "No packet available, nonblocking socket"
	case NextError:
		return "Generic error"
	case NextOk:
		return "Success (not an error)"
	case NextNotEnabled:
		return "Ring not enabled"
	}
	return strconv.Itoa(int(n))
}

// shared code (Read-functions), that fetches a packet + metadata from pf_ring
func (r *Ring) getNextBufPtrLocked(ci *gopacket.CaptureInfo) error {
	result := NextResult(C.pfring_readpacketdatato_wrapper(r.cptr, C.uintptr_t(uintptr(unsafe.Pointer(&r.bufPtr))), C.uintptr_t(uintptr(unsafe.Pointer(&r.meta)))))
	if result != NextOk {
		return result
	}
	ci.Timestamp = time.Unix(0, int64(r.meta.timestamp_ns))
	ci.CaptureLength = int(r.meta.caplen)
	ci.Length = int(r.meta.len)
	if r.useExtendedPacketHeader {
		ci.InterfaceIndex = int(r.meta.if_index)
	} else {
		ci.InterfaceIndex = r.interfaceIndex
	}
	return nil
}

// ReadPacketDataTo reads packet data into a user-supplied buffer.
//
// Deprecated: This function is provided for legacy code only. Use ReadPacketData or ZeroCopyReadPacketData
// This function does an additional copy, and is therefore slower than ZeroCopyReadPacketData.
// The old implementation did the same inside the pf_ring library.
func (r *Ring) ReadPacketDataTo(data []byte) (ci gopacket.CaptureInfo, err error) {
	r.mu.Lock()
	err = r.getNextBufPtrLocked(&ci)
	if err == nil {
		var buf []byte
		slice := (*reflect.SliceHeader)(unsafe.Pointer(&buf))
		slice.Data = uintptr(unsafe.Pointer(r.bufPtr))
		slice.Len = ci.CaptureLength
		slice.Cap = ci.CaptureLength
		copy(data, buf)
	}
	r.mu.Unlock()
	return
}

// ReadPacketData returns the next packet read from pf_ring, along with an error
// code associated with that packet. If the packet is read successfully, the
// returned error is nil.
func (r *Ring) ReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	r.mu.Lock()
	err = r.getNextBufPtrLocked(&ci)
	if err == nil {
		data = C.GoBytes(unsafe.Pointer(r.bufPtr), C.int(ci.CaptureLength))
	}
	r.mu.Unlock()
	return
}

// ZeroCopyReadPacketData returns the next packet read from pf_ring, along with an error
// code associated with that packet.
// The slice returned by ZeroCopyReadPacketData points to bytes inside a pf_ring
// ring. Each call to ZeroCopyReadPacketData might invalidate any data previously
// returned by ZeroCopyReadPacketData. Care must be taken not to keep pointers
// to old bytes when using ZeroCopyReadPacketData... if you need to keep data past
// the next time you call ZeroCopyReadPacketData, use ReadPacketData, which copies
// the bytes into a new buffer for you.
//  data1, _, _ := handle.ZeroCopyReadPacketData()
//  // do everything you want with data1 here, copying bytes out of it if you'd like to keep them around.
//  data2, _, _ := handle.ZeroCopyReadPacketData()  // invalidates bytes in data1
func (r *Ring) ZeroCopyReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	r.mu.Lock()
	err = r.getNextBufPtrLocked(&ci)
	if err == nil {
		slice := (*reflect.SliceHeader)(unsafe.Pointer(&data))
		slice.Data = uintptr(unsafe.Pointer(r.bufPtr))
		slice.Len = ci.CaptureLength
		slice.Cap = ci.CaptureLength
	}
	r.mu.Unlock()
	return
}

// ClusterType is a type of clustering used when balancing across multiple
// rings.
type ClusterType C.cluster_type

const (
	// ClusterPerFlow clusters by <src ip, src port, dst ip, dst port, proto,
	// vlan>
	ClusterPerFlow ClusterType = C.cluster_per_flow
	// ClusterRoundRobin round-robins packets between applications, ignoring
	// packet information.
	ClusterRoundRobin ClusterType = C.cluster_round_robin
	// ClusterPerFlow2Tuple clusters by <src ip, dst ip>
	ClusterPerFlow2Tuple ClusterType = C.cluster_per_flow_2_tuple
	// ClusterPerFlow4Tuple clusters by <src ip, src port, dst ip, dst port>
	ClusterPerFlow4Tuple ClusterType = C.cluster_per_flow_4_tuple
	// ClusterPerFlow5Tuple clusters by <src ip, src port, dst ip, dst port,
	// proto>
	ClusterPerFlow5Tuple ClusterType = C.cluster_per_flow_5_tuple
	// ClusterPerFlowTCP5Tuple acts like ClusterPerFlow5Tuple for TCP packets and
	// like ClusterPerFlow2Tuple for all other packets.
	ClusterPerFlowTCP5Tuple ClusterType = C.cluster_per_flow_tcp_5_tuple
)

// SetCluster sets which cluster the ring should be part of, and the cluster
// type to use.
func (r *Ring) SetCluster(cluster int, typ ClusterType) error {
	if rv := C.pfring_set_cluster(r.cptr, C.u_int(cluster), C.cluster_type(typ)); rv != 0 {
		return fmt.Errorf("Unable to set cluster, got error code %d", rv)
	}
	return nil
}

// RemoveFromCluster removes the ring from the cluster it was put in with
// SetCluster.
func (r *Ring) RemoveFromCluster() error {
	if rv := C.pfring_remove_from_cluster(r.cptr); rv != 0 {
		return fmt.Errorf("Unable to remove from cluster, got error code %d", rv)
	}
	return nil
}

// SetSamplingRate sets the sampling rate to 1/<rate>.
func (r *Ring) SetSamplingRate(rate int) error {
	if rv := C.pfring_set_sampling_rate(r.cptr, C.u_int32_t(rate)); rv != 0 {
		return fmt.Errorf("Unable to set sampling rate, got error code %d", rv)
	}
	return nil
}

// SetPollWatermark sets the pfring's poll watermark packet count
func (r *Ring) SetPollWatermark(count uint16) error {
	if rv := C.pfring_set_poll_watermark(r.cptr, C.u_int16_t(count)); rv != 0 {
		return fmt.Errorf("Unable to set poll watermark, got error code %d", rv)
	}
	return nil
}

// SetPriority sets the pfring poll threads CPU usage limit
func (r *Ring) SetPriority(cpu uint16) {
	C.pfring_config(C.u_short(cpu))
}

// SetPollDuration sets the pfring's poll duration before it yields/returns
func (r *Ring) SetPollDuration(durationMillis uint) error {
	if rv := C.pfring_set_poll_duration(r.cptr, C.u_int(durationMillis)); rv != 0 {
		return fmt.Errorf("Unable to set poll duration, got error code %d", rv)
	}
	return nil
}

// SetBPFFilter sets the BPF filter for the ring.
func (r *Ring) SetBPFFilter(bpfFilter string) error {
	filter := C.CString(bpfFilter)
	defer C.free(unsafe.Pointer(filter))
	if rv := C.pfring_set_bpf_filter(r.cptr, filter); rv != 0 {
		return fmt.Errorf("Unable to set BPF filter, got error code %d", rv)
	}
	return nil
}

// RemoveBPFFilter removes the BPF filter from the ring.
func (r *Ring) RemoveBPFFilter() error {
	if rv := C.pfring_remove_bpf_filter(r.cptr); rv != 0 {
		return fmt.Errorf("Unable to remove BPF filter, got error code %d", rv)
	}
	return nil
}

// WritePacketData uses the ring to send raw packet data to the interface.
func (r *Ring) WritePacketData(data []byte) error {
	buf := (*C.char)(unsafe.Pointer(&data[0]))
	if rv := C.pfring_send(r.cptr, buf, C.u_int(len(data)), 1); rv < 0 {
		return fmt.Errorf("Unable to send packet data, got error code %d", rv)
	}
	return nil
}

// Enable enables the given ring.  This function MUST be called on each new
// ring after it has been set up, or that ring will NOT receive packets.
func (r *Ring) Enable() error {
	if rv := C.pfring_enable_ring(r.cptr); rv != 0 {
		return fmt.Errorf("Unable to enable ring, got error code %d", rv)
	}
	return nil
}

// Disable disables the given ring.  After this call, it will no longer receive
// packets.
func (r *Ring) Disable() error {
	if rv := C.pfring_disable_ring(r.cptr); rv != 0 {
		return fmt.Errorf("Unable to disable ring, got error code %d", rv)
	}
	return nil
}

// Stats provides simple statistics on a ring.
type Stats struct {
	Received, Dropped uint64
}

// Stats returns statistsics for the ring.
func (r *Ring) Stats() (s Stats, err error) {
	var stats C.pfring_stat
	if rv := C.pfring_stats(r.cptr, &stats); rv != 0 {
		err = fmt.Errorf("Unable to get ring stats, got error code %d", rv)
		return
	}
	s.Received = uint64(stats.recv)
	s.Dropped = uint64(stats.drop)
	return
}

// Direction is a simple enum to set which packets (TX, RX, or both) a ring
// captures.
type Direction C.packet_direction

const (
	// TransmitOnly will only capture packets transmitted by the ring's
	// interface(s).
	TransmitOnly Direction = C.tx_only_direction
	// ReceiveOnly will only capture packets received by the ring's
	// interface(s).
	ReceiveOnly Direction = C.rx_only_direction
	// ReceiveAndTransmit will capture both received and transmitted packets on
	// the ring's interface(s).
	ReceiveAndTransmit Direction = C.rx_and_tx_direction
)

// SetDirection sets which packets should be captured by the ring.
func (r *Ring) SetDirection(d Direction) error {
	if rv := C.pfring_set_direction(r.cptr, C.packet_direction(d)); rv != 0 {
		return fmt.Errorf("Unable to set ring direction, got error code %d", rv)
	}
	return nil
}

// SocketMode is an enum for setting whether a ring should read, write, or both.
type SocketMode C.socket_mode

const (
	// WriteOnly sets up the ring to only send packets (Inject), not read them.
	WriteOnly SocketMode = C.send_only_mode
	// ReadOnly sets up the ring to only receive packets (ReadPacketData), not
	// send them.
	ReadOnly SocketMode = C.recv_only_mode
	// WriteAndRead sets up the ring to both send and receive packets.
	WriteAndRead SocketMode = C.send_and_recv_mode
)

// SetSocketMode sets the mode of the ring socket to send, receive, or both.
func (r *Ring) SetSocketMode(s SocketMode) error {
	if rv := C.pfring_set_socket_mode(r.cptr, C.socket_mode(s)); rv != 0 {
		return fmt.Errorf("Unable to set socket mode, got error code %d", rv)
	}
	return nil
}

// SetApplicationName sets a string name to the ring.  This name is available in
// /proc stats for pf_ring.  By default, NewRing automatically calls this with
// argv[0].
func (r *Ring) SetApplicationName(name string) error {
	buf := C.CString(name)
	defer C.free(unsafe.Pointer(buf))
	if rv := C.pfring_set_application_name(r.cptr, buf); rv != 0 {
		return fmt.Errorf("Unable to set ring application name, got error code %d", rv)
	}
	return nil
}
