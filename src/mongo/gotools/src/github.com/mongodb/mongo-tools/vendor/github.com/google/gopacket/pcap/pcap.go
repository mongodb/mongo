// Copyright 2012 Google, Inc. All rights reserved.
// Copyright 2009-2011 Andreas Krennmair. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pcap

import (
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"reflect"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

// ErrNotActive is returned if handle is not activated
const ErrNotActive = pcapErrorNotActivated

// MaxBpfInstructions is the maximum number of BPF instructions supported (BPF_MAXINSNS),
// taken from Linux kernel: include/uapi/linux/bpf_common.h
//
// https://github.com/torvalds/linux/blob/master/include/uapi/linux/bpf_common.h
const MaxBpfInstructions = 4096

// 8 bytes per instruction, max 4096 instructions
const bpfInstructionBufferSize = 8 * MaxBpfInstructions

// Handle provides a connection to a pcap handle, allowing users to read packets
// off the wire (Next), inject packets onto the wire (Inject), and
// perform a number of other functions to affect and understand packet output.
//
// Handles are already pcap_activate'd
type Handle struct {
	// stop is set to a non-zero value by Handle.Close to signal to
	// getNextBufPtrLocked to stop trying to read packets
	// This must be the first entry to ensure alignment for sync.atomic
	stop uint64
	// cptr is the handle for the actual pcap C object.
	cptr           pcapTPtr
	timeout        time.Duration
	device         string
	deviceIndex    int
	mu             sync.Mutex
	closeMu        sync.Mutex
	nanoSecsFactor int64

	// Since pointers to these objects are passed into a C function, if
	// they're declared locally then the Go compiler thinks they may have
	// escaped into C-land, so it allocates them on the heap.  This causes a
	// huge memory hit, so to handle that we store them here instead.
	pkthdr *pcapPkthdr
	bufptr *uint8
}

// Stats contains statistics on how many packets were handled by a pcap handle,
// and what was done with those packets.
type Stats struct {
	PacketsReceived  int
	PacketsDropped   int
	PacketsIfDropped int
}

// Interface describes a single network interface on a machine.
type Interface struct {
	Name        string
	Description string
	Flags       uint32
	Addresses   []InterfaceAddress
}

// Datalink describes the datalink
type Datalink struct {
	Name        string
	Description string
}

// InterfaceAddress describes an address associated with an Interface.
// Currently, it's IPv4/6 specific.
type InterfaceAddress struct {
	IP        net.IP
	Netmask   net.IPMask // Netmask may be nil if we were unable to retrieve it.
	Broadaddr net.IP     // Broadcast address for this IP may be nil
	P2P       net.IP     // P2P destination address for this IP may be nil
}

// BPF is a compiled filter program, useful for offline packet matching.
type BPF struct {
	orig string
	bpf  pcapBpfProgram // takes a finalizer, not overriden by outsiders
	hdr  pcapPkthdr     // allocate on the heap to enable optimizations
}

// BPFInstruction is a byte encoded structure holding a BPF instruction
type BPFInstruction struct {
	Code uint16
	Jt   uint8
	Jf   uint8
	K    uint32
}

// BlockForever causes it to block forever waiting for packets, when passed
// into SetTimeout or OpenLive, while still returning incoming packets to userland relatively
// quickly.
const BlockForever = -time.Millisecond * 10

func timeoutMillis(timeout time.Duration) int {
	// Flip sign if necessary.  See package docs on timeout for reasoning behind this.
	if timeout < 0 {
		timeout *= -1
	}
	// Round up
	if timeout != 0 && timeout < time.Millisecond {
		timeout = time.Millisecond
	}
	return int(timeout / time.Millisecond)
}

// OpenLive opens a device and returns a *Handle.
// It takes as arguments the name of the device ("eth0"), the maximum size to
// read for each packet (snaplen), whether to put the interface in promiscuous
// mode, and a timeout. Warning: this function supports only microsecond timestamps.
// For nanosecond resolution use an InactiveHandle.
//
// See the package documentation for important details regarding 'timeout'.
func OpenLive(device string, snaplen int32, promisc bool, timeout time.Duration) (handle *Handle, _ error) {
	var pro int
	if promisc {
		pro = 1
	}

	p, err := pcapOpenLive(device, int(snaplen), pro, timeoutMillis(timeout))
	if err != nil {
		return nil, err
	}
	p.timeout = timeout
	p.device = device

	ifc, err := net.InterfaceByName(device)
	if err != nil {
		// The device wasn't found in the OS, but could be "any"
		// Set index to 0
		p.deviceIndex = 0
	} else {
		p.deviceIndex = ifc.Index
	}

	p.nanoSecsFactor = 1000

	// Only set the PCAP handle into non-blocking mode if we have a timeout
	// greater than zero. If the user wants to block forever, we'll let libpcap
	// handle that.
	if p.timeout > 0 {
		if err := p.setNonBlocking(); err != nil {
			p.pcapClose()
			return nil, err
		}
	}

	return p, nil
}

// OpenOffline opens a file and returns its contents as a *Handle. Depending on libpcap support and
// on the timestamp resolution used in the file, nanosecond or microsecond resolution is used
// internally. All returned timestamps are scaled to nanosecond resolution. Resolution() can be used
// to query the actual resolution used.
func OpenOffline(file string) (handle *Handle, err error) {
	handle, err = openOffline(file)
	if err != nil {
		return
	}
	if pcapGetTstampPrecision(handle.cptr) == pcapTstampPrecisionNano {
		handle.nanoSecsFactor = 1
	} else {
		handle.nanoSecsFactor = 1000
	}
	return
}

// OpenOfflineFile returns contents of input file as a *Handle. Depending on libpcap support and
// on the timestamp resolution used in the file, nanosecond or microsecond resolution is used
// internally. All returned timestamps are scaled to nanosecond resolution. Resolution() can be used
// to query the actual resolution used.
func OpenOfflineFile(file *os.File) (handle *Handle, err error) {
	handle, err = openOfflineFile(file)
	if err != nil {
		return
	}
	if pcapGetTstampPrecision(handle.cptr) == pcapTstampPrecisionNano {
		handle.nanoSecsFactor = 1
	} else {
		handle.nanoSecsFactor = 1000
	}
	return
}

// NextError is the return code from a call to Next.
type NextError int32

// NextError implements the error interface.
func (n NextError) Error() string {
	switch n {
	case NextErrorOk:
		return "OK"
	case NextErrorTimeoutExpired:
		return "Timeout Expired"
	case NextErrorReadError:
		return "Read Error"
	case NextErrorNoMorePackets:
		return "No More Packets In File"
	case NextErrorNotActivated:
		return "Not Activated"
	}
	return strconv.Itoa(int(n))
}

// NextError values.
const (
	NextErrorOk             NextError = 1
	NextErrorTimeoutExpired NextError = 0
	NextErrorReadError      NextError = -1
	// NextErrorNoMorePackets is returned when reading from a file (OpenOffline) and
	// EOF is reached.  When this happens, Next() returns io.EOF instead of this.
	NextErrorNoMorePackets NextError = -2
	NextErrorNotActivated  NextError = -3
)

// ReadPacketData returns the next packet read from the pcap handle, along with an error
// code associated with that packet.  If the packet is read successfully, the
// returned error is nil.
func (p *Handle) ReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	p.mu.Lock()
	err = p.getNextBufPtrLocked(&ci)
	if err == nil {
		data = make([]byte, ci.CaptureLength)
		copy(data, (*(*[1 << 30]byte)(unsafe.Pointer(p.bufptr)))[:])
	}
	p.mu.Unlock()
	if err == NextErrorTimeoutExpired {
		runtime.Gosched()
	}
	return
}

type activateError int

const (
	aeNoError      = activateError(0)
	aeActivated    = activateError(pcapErrorActivated)
	aePromisc      = activateError(pcapWarningPromisc)
	aeNoSuchDevice = activateError(pcapErrorNoSuchDevice)
	aeDenied       = activateError(pcapErrorDenied)
	aeNotUp        = activateError(pcapErrorNotUp)
	aeWarning      = activateError(pcapWarning)
)

func (a activateError) Error() string {
	switch a {
	case aeNoError:
		return "No Error"
	case aeActivated:
		return "Already Activated"
	case aePromisc:
		return "Cannot set as promisc"
	case aeNoSuchDevice:
		return "No Such Device"
	case aeDenied:
		return "Permission Denied"
	case aeNotUp:
		return "Interface Not Up"
	case aeWarning:
		return fmt.Sprintf("Warning: %v", activateErrMsg.Error())
	default:
		return fmt.Sprintf("unknown activated error: %d", a)
	}
}

// getNextBufPtrLocked is shared code for ReadPacketData and
// ZeroCopyReadPacketData.
func (p *Handle) getNextBufPtrLocked(ci *gopacket.CaptureInfo) error {
	if !p.isOpen() {
		return io.EOF
	}

	// set after we have call waitForPacket for the first time
	var waited bool

	for atomic.LoadUint64(&p.stop) == 0 {
		// try to read a packet if one is immediately available
		result := p.pcapNextPacketEx()

		switch result {
		case NextErrorOk:
			sec := p.pkthdr.getSec()
			// convert micros to nanos
			nanos := int64(p.pkthdr.getUsec()) * p.nanoSecsFactor

			ci.Timestamp = time.Unix(sec, nanos)
			ci.CaptureLength = p.pkthdr.getCaplen()
			ci.Length = p.pkthdr.getLen()
			ci.InterfaceIndex = p.deviceIndex

			return nil
		case NextErrorNoMorePackets:
			// no more packets, return EOF rather than libpcap-specific error
			return io.EOF
		case NextErrorTimeoutExpired:
			// we've already waited for a packet and we're supposed to time out
			//
			// we should never actually hit this if we were passed BlockForever
			// since we should block on C.pcap_next_ex until there's a packet
			// to read.
			if waited && p.timeout > 0 {
				return result
			}

			// wait for packet before trying again
			p.waitForPacket()
			waited = true
		default:
			return result
		}
	}

	// stop must be set
	return io.EOF
}

// ZeroCopyReadPacketData reads the next packet off the wire, and returns its data.
// The slice returned by ZeroCopyReadPacketData points to bytes owned by the
// the Handle.  Each call to ZeroCopyReadPacketData invalidates any data previously
// returned by ZeroCopyReadPacketData.  Care must be taken not to keep pointers
// to old bytes when using ZeroCopyReadPacketData... if you need to keep data past
// the next time you call ZeroCopyReadPacketData, use ReadPacketData, which copies
// the bytes into a new buffer for you.
//  data1, _, _ := handle.ZeroCopyReadPacketData()
//  // do everything you want with data1 here, copying bytes out of it if you'd like to keep them around.
//  data2, _, _ := handle.ZeroCopyReadPacketData()  // invalidates bytes in data1
func (p *Handle) ZeroCopyReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	p.mu.Lock()
	err = p.getNextBufPtrLocked(&ci)
	if err == nil {
		slice := (*reflect.SliceHeader)(unsafe.Pointer(&data))
		slice.Data = uintptr(unsafe.Pointer(p.bufptr))
		slice.Len = ci.CaptureLength
		slice.Cap = ci.CaptureLength
	}
	p.mu.Unlock()
	if err == NextErrorTimeoutExpired {
		runtime.Gosched()
	}
	return
}

// Close closes the underlying pcap handle.
func (p *Handle) Close() {
	p.closeMu.Lock()
	defer p.closeMu.Unlock()

	if !p.isOpen() {
		return
	}

	atomic.StoreUint64(&p.stop, 1)

	// wait for packet reader to stop
	p.mu.Lock()
	defer p.mu.Unlock()

	p.pcapClose()
}

// Error returns the current error associated with a pcap handle (pcap_geterr).
func (p *Handle) Error() error {
	return p.pcapGeterr()
}

// Stats returns statistics on the underlying pcap handle.
func (p *Handle) Stats() (stat *Stats, err error) {
	return p.pcapStats()
}

// ListDataLinks obtains a list of all possible data link types supported for an interface.
func (p *Handle) ListDataLinks() (datalinks []Datalink, err error) {
	return p.pcapListDatalinks()
}

// compileBPFFilter always returns an allocated C.struct_bpf_program
// It is the callers responsibility to free the memory again, e.g.
//
//    C.pcap_freecode(&bpf)
//
func (p *Handle) compileBPFFilter(expr string) (pcapBpfProgram, error) {
	var maskp = uint32(pcapNetmaskUnknown)

	// Only do the lookup on network interfaces.
	// No device indicates we're handling a pcap file.
	if len(p.device) > 0 {
		var err error
		_, maskp, err = pcapLookupnet(p.device)
		if err != nil {
			// We can't lookup the network, but that could be because the interface
			// doesn't have an IPv4.
			maskp = uint32(pcapNetmaskUnknown)
		}
	}

	return p.pcapCompile(expr, maskp)
}

// CompileBPFFilter compiles and returns a BPF filter with given a link type and capture length.
func CompileBPFFilter(linkType layers.LinkType, captureLength int, expr string) ([]BPFInstruction, error) {
	h, err := pcapOpenDead(linkType, captureLength)
	if err != nil {
		return nil, err
	}
	defer h.Close()
	return h.CompileBPFFilter(expr)
}

// CompileBPFFilter compiles and returns a BPF filter for the pcap handle.
func (p *Handle) CompileBPFFilter(expr string) ([]BPFInstruction, error) {
	bpf, err := p.compileBPFFilter(expr)
	defer bpf.free()
	if err != nil {
		return nil, err
	}

	return bpf.toBPFInstruction(), nil
}

// SetBPFFilter compiles and sets a BPF filter for the pcap handle.
func (p *Handle) SetBPFFilter(expr string) (err error) {
	bpf, err := p.compileBPFFilter(expr)
	defer bpf.free()
	if err != nil {
		return err
	}

	return p.pcapSetfilter(bpf)
}

// SetBPFInstructionFilter may be used to apply a filter in BPF asm byte code format.
//
// Simplest way to generate BPF asm byte code is with tcpdump:
//     tcpdump -dd 'udp'
//
// The output may be used directly to add a filter, e.g.:
//     bpfInstructions := []pcap.BpfInstruction{
//			{0x28, 0, 0, 0x0000000c},
//			{0x15, 0, 9, 0x00000800},
//			{0x30, 0, 0, 0x00000017},
//			{0x15, 0, 7, 0x00000006},
//			{0x28, 0, 0, 0x00000014},
//			{0x45, 5, 0, 0x00001fff},
//			{0xb1, 0, 0, 0x0000000e},
//			{0x50, 0, 0, 0x0000001b},
//			{0x54, 0, 0, 0x00000012},
//			{0x15, 0, 1, 0x00000012},
//			{0x6, 0, 0, 0x0000ffff},
//			{0x6, 0, 0, 0x00000000},
//		}
//
// An other posibility is to write the bpf code in bpf asm.
// Documentation: https://www.kernel.org/doc/Documentation/networking/filter.txt
//
// To compile the code use bpf_asm from
// https://github.com/torvalds/linux/tree/master/tools/net
//
// The following command may be used to convert bpf_asm output to c/go struct, usable for SetBPFFilterByte:
// bpf_asm -c tcp.bpf
func (p *Handle) SetBPFInstructionFilter(bpfInstructions []BPFInstruction) (err error) {
	bpf, err := bpfInstructionFilter(bpfInstructions)
	if err != nil {
		return err
	}
	defer bpf.free()

	return p.pcapSetfilter(bpf)
}

func bpfInstructionFilter(bpfInstructions []BPFInstruction) (bpf pcapBpfProgram, err error) {
	if len(bpfInstructions) < 1 {
		return bpf, errors.New("bpfInstructions must not be empty")
	}

	if len(bpfInstructions) > MaxBpfInstructions {
		return bpf, fmt.Errorf("bpfInstructions must not be larger than %d", MaxBpfInstructions)
	}

	return pcapBpfProgramFromInstructions(bpfInstructions), nil
}

// NewBPF compiles the given string into a new filter program.
//
// BPF filters need to be created from activated handles, because they need to
// know the underlying link type to correctly compile their offsets.
func (p *Handle) NewBPF(expr string) (*BPF, error) {
	bpf := &BPF{orig: expr}

	var err error
	bpf.bpf, err = p.pcapCompile(expr, pcapNetmaskUnknown)
	if err != nil {
		return nil, err
	}

	runtime.SetFinalizer(bpf, destroyBPF)
	return bpf, nil
}

// NewBPF allows to create a BPF without requiring an existing handle.
// This allows to match packets obtained from a-non GoPacket capture source
// to be matched.
//
// buf := make([]byte, MaxFrameSize)
// bpfi, _ := pcap.NewBPF(layers.LinkTypeEthernet, MaxFrameSize, "icmp")
// n, _ := someIO.Read(buf)
// ci := gopacket.CaptureInfo{CaptureLength: n, Length: n}
// if bpfi.Matches(ci, buf) {
//     doSomething()
// }
func NewBPF(linkType layers.LinkType, captureLength int, expr string) (*BPF, error) {
	h, err := pcapOpenDead(linkType, captureLength)
	if err != nil {
		return nil, err
	}
	defer h.Close()
	return h.NewBPF(expr)
}

// NewBPFInstructionFilter sets the given BPFInstructions as new filter program.
//
// More details see func SetBPFInstructionFilter
//
// BPF filters need to be created from activated handles, because they need to
// know the underlying link type to correctly compile their offsets.
func (p *Handle) NewBPFInstructionFilter(bpfInstructions []BPFInstruction) (*BPF, error) {
	var err error
	bpf := &BPF{orig: "BPF Instruction Filter"}

	bpf.bpf, err = bpfInstructionFilter(bpfInstructions)
	if err != nil {
		return nil, err
	}

	runtime.SetFinalizer(bpf, destroyBPF)
	return bpf, nil
}
func destroyBPF(bpf *BPF) {
	bpf.bpf.free()
}

// String returns the original string this BPF filter was compiled from.
func (b *BPF) String() string {
	return b.orig
}

// Matches returns true if the given packet data matches this filter.
func (b *BPF) Matches(ci gopacket.CaptureInfo, data []byte) bool {
	return b.pcapOfflineFilter(ci, data)
}

// Version returns pcap_lib_version.
func Version() string {
	return pcapLibVersion()
}

// LinkType returns pcap_datalink, as a layers.LinkType.
func (p *Handle) LinkType() layers.LinkType {
	return p.pcapDatalink()
}

// SetLinkType calls pcap_set_datalink on the pcap handle.
func (p *Handle) SetLinkType(dlt layers.LinkType) error {
	return p.pcapSetDatalink(dlt)
}

// DatalinkValToName returns pcap_datalink_val_to_name as string
func DatalinkValToName(dlt int) string {
	return pcapDatalinkValToName(dlt)
}

// DatalinkValToDescription returns pcap_datalink_val_to_description as string
func DatalinkValToDescription(dlt int) string {
	return pcapDatalinkValToDescription(dlt)
}

// DatalinkNameToVal returns pcap_datalink_name_to_val as int
func DatalinkNameToVal(name string) int {
	return pcapDatalinkNameToVal(name)
}

// FindAllDevs attempts to enumerate all interfaces on the current machine.
func FindAllDevs() (ifs []Interface, err error) {
	alldevsp, err := pcapFindAllDevs()
	if err != nil {
		return nil, err
	}
	defer alldevsp.free()

	for alldevsp.next() {
		var iface Interface
		iface.Name = alldevsp.name()
		iface.Description = alldevsp.description()
		iface.Addresses = findalladdresses(alldevsp.addresses())
		iface.Flags = alldevsp.flags()
		ifs = append(ifs, iface)
	}
	return
}

func findalladdresses(addresses pcapAddresses) (retval []InterfaceAddress) {
	// TODO - make it support more than IPv4 and IPv6?
	retval = make([]InterfaceAddress, 0, 1)
	for addresses.next() {
		// Strangely, it appears that in some cases, we get a pcap address back from
		// pcap_findalldevs with a nil .addr.  It appears that we can skip over
		// these.
		if addresses.addr() == nil {
			continue
		}
		var a InterfaceAddress
		var err error
		if a.IP, err = sockaddrToIP(addresses.addr()); err != nil {
			continue
		}
		// To be safe, we'll also check for netmask.
		if addresses.netmask() == nil {
			continue
		}
		if a.Netmask, err = sockaddrToIP(addresses.netmask()); err != nil {
			// If we got an IP address but we can't get a netmask, just return the IP
			// address.
			a.Netmask = nil
		}
		if a.Broadaddr, err = sockaddrToIP(addresses.broadaddr()); err != nil {
			a.Broadaddr = nil
		}
		if a.P2P, err = sockaddrToIP(addresses.dstaddr()); err != nil {
			a.P2P = nil
		}
		retval = append(retval, a)
	}
	return
}

func sockaddrToIP(rsa *syscall.RawSockaddr) (IP []byte, err error) {
	if rsa == nil {
		err = errors.New("Value not set")
		return
	}
	switch rsa.Family {
	case syscall.AF_INET:
		pp := (*syscall.RawSockaddrInet4)(unsafe.Pointer(rsa))
		IP = make([]byte, 4)
		for i := 0; i < len(IP); i++ {
			IP[i] = pp.Addr[i]
		}
		return
	case syscall.AF_INET6:
		pp := (*syscall.RawSockaddrInet6)(unsafe.Pointer(rsa))
		IP = make([]byte, 16)
		for i := 0; i < len(IP); i++ {
			IP[i] = pp.Addr[i]
		}
		return
	}
	err = errors.New("Unsupported address type")
	return
}

// WritePacketData calls pcap_sendpacket, injecting the given data into the pcap handle.
func (p *Handle) WritePacketData(data []byte) (err error) {
	return p.pcapSendpacket(data)
}

// Direction is used by Handle.SetDirection.
type Direction uint8

// Direction values for Handle.SetDirection.
const (
	DirectionIn    = Direction(pcapDIN)
	DirectionOut   = Direction(pcapDOUT)
	DirectionInOut = Direction(pcapDINOUT)
)

// SetDirection sets the direction for which packets will be captured.
func (p *Handle) SetDirection(direction Direction) error {
	if direction != DirectionIn && direction != DirectionOut && direction != DirectionInOut {
		return fmt.Errorf("Invalid direction: %v", direction)
	}
	return p.pcapSetdirection(direction)
}

// SnapLen returns the snapshot length
func (p *Handle) SnapLen() int {
	return p.pcapSnapshot()
}

// Resolution returns the timestamp resolution of acquired timestamps before scaling to NanosecondTimestampResolution.
func (p *Handle) Resolution() gopacket.TimestampResolution {
	if p.nanoSecsFactor == 1 {
		return gopacket.TimestampResolutionMicrosecond
	}
	return gopacket.TimestampResolutionNanosecond
}

// TimestampSource tells PCAP which type of timestamp to use for packets.
type TimestampSource int

// String returns the timestamp type as a human-readable string.
func (t TimestampSource) String() string {
	return t.pcapTstampTypeValToName()
}

// TimestampSourceFromString translates a string into a timestamp type, case
// insensitive.
func TimestampSourceFromString(s string) (TimestampSource, error) {
	return pcapTstampTypeNameToVal(s)
}

// InactiveHandle allows you to call pre-pcap_activate functions on your pcap
// handle to set it up just the way you'd like.
type InactiveHandle struct {
	// cptr is the handle for the actual pcap C object.
	cptr        pcapTPtr
	device      string
	deviceIndex int
	timeout     time.Duration
}

// holds the err messoge in case activation returned a Warning
var activateErrMsg error

// Error returns the current error associated with a pcap handle (pcap_geterr).
func (p *InactiveHandle) Error() error {
	return p.pcapGeterr()
}

// Activate activates the handle.  The current InactiveHandle becomes invalid
// and all future function calls on it will fail.
func (p *InactiveHandle) Activate() (*Handle, error) {
	// ignore error with set_tstamp_precision, since the actual precision is queried later anyway
	pcapSetTstampPrecision(p.cptr, pcapTstampPrecisionNano)
	handle, err := p.pcapActivate()
	if err != aeNoError {
		if err == aeWarning {
			activateErrMsg = p.Error()
		}
		return nil, err
	}
	handle.timeout = p.timeout
	if p.timeout > 0 {
		if err := handle.setNonBlocking(); err != nil {
			handle.pcapClose()
			return nil, err
		}
	}
	handle.device = p.device
	handle.deviceIndex = p.deviceIndex
	if pcapGetTstampPrecision(handle.cptr) == pcapTstampPrecisionNano {
		handle.nanoSecsFactor = 1
	} else {
		handle.nanoSecsFactor = 1000
	}
	return handle, nil
}

// CleanUp cleans up any stuff left over from a successful or failed building
// of a handle.
func (p *InactiveHandle) CleanUp() {
	p.pcapClose()
}

// NewInactiveHandle creates a new InactiveHandle, which wraps an un-activated PCAP handle.
// Callers of NewInactiveHandle should immediately defer 'CleanUp', as in:
//   inactive := NewInactiveHandle("eth0")
//   defer inactive.CleanUp()
func NewInactiveHandle(device string) (*InactiveHandle, error) {
	// Try to get the interface index, but iy could be something like "any"
	// in which case use 0, which doesn't exist in nature
	deviceIndex := 0
	ifc, err := net.InterfaceByName(device)
	if err == nil {
		deviceIndex = ifc.Index
	}

	// This copies a bunch of the pcap_open_live implementation from pcap.c:
	handle, err := pcapCreate(device)
	if err != nil {
		return nil, err
	}
	handle.device = device
	handle.deviceIndex = deviceIndex
	return handle, nil
}

// SetSnapLen sets the snap length (max bytes per packet to capture).
func (p *InactiveHandle) SetSnapLen(snaplen int) error {
	return p.pcapSetSnaplen(snaplen)
}

// SetPromisc sets the handle to either be promiscuous (capture packets
// unrelated to this host) or not.
func (p *InactiveHandle) SetPromisc(promisc bool) error {
	return p.pcapSetPromisc(promisc)
}

// SetTimeout sets the read timeout for the handle.
//
// See the package documentation for important details regarding 'timeout'.
func (p *InactiveHandle) SetTimeout(timeout time.Duration) error {
	err := p.pcapSetTimeout(timeout)
	if err != nil {
		return err
	}
	p.timeout = timeout
	return nil
}

// SupportedTimestamps returns a list of supported timstamp types for this
// handle.
func (p *InactiveHandle) SupportedTimestamps() (out []TimestampSource) {
	return p.pcapListTstampTypes()
}

// SetTimestampSource sets the type of timestamp generator PCAP uses when
// attaching timestamps to packets.
func (p *InactiveHandle) SetTimestampSource(t TimestampSource) error {
	return p.pcapSetTstampType(t)
}

// CannotSetRFMon is returned by SetRFMon if the handle does not allow
// setting RFMon because pcap_can_set_rfmon returns 0.
var CannotSetRFMon = errors.New("Cannot set rfmon for this handle")

// SetRFMon turns on radio monitoring mode, similar to promiscuous mode but for
// wireless networks.  If this mode is enabled, the interface will not need to
// associate with an access point before it can receive traffic.
func (p *InactiveHandle) SetRFMon(monitor bool) error {
	return p.pcapSetRfmon(monitor)
}

// SetBufferSize sets the buffer size (in bytes) of the handle.
func (p *InactiveHandle) SetBufferSize(bufferSize int) error {
	return p.pcapSetBufferSize(bufferSize)
}

// SetImmediateMode sets (or unsets) the immediate mode of the
// handle. In immediate mode, packets are delivered to the application
// as soon as they arrive.  In other words, this overrides SetTimeout.
func (p *InactiveHandle) SetImmediateMode(mode bool) error {
	return p.pcapSetImmediateMode(mode)
}
