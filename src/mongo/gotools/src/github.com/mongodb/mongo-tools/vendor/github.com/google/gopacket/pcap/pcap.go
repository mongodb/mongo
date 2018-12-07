// Copyright 2012 Google, Inc. All rights reserved.
// Copyright 2009-2011 Andreas Krennmair. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pcap

/*
#cgo linux LDFLAGS: -lpcap
#cgo freebsd LDFLAGS: -lpcap
#cgo openbsd LDFLAGS: -lpcap
#cgo darwin LDFLAGS: -lpcap
#cgo windows CFLAGS: -I C:/WpdPack/Include
#cgo windows,386 LDFLAGS: -L C:/WpdPack/Lib -lwpcap
#cgo windows,amd64 LDFLAGS: -L C:/WpdPack/Lib/x64 -lwpcap
#include <stdlib.h>
#include <pcap.h>

// Some old versions of pcap don't define this constant.
#ifndef PCAP_NETMASK_UNKNOWN
#define PCAP_NETMASK_UNKNOWN 0xffffffff
#endif

// libpcap doesn't actually export its version in a #define-guardable way,
// so we have to use other defined things to differentiate versions.
// We assume at least libpcap v1.1 at the moment.
// See http://upstream-tracker.org/versions/libpcap.html

#ifndef PCAP_ERROR_TSTAMP_PRECISION_NOTSUP  // < v1.5

int pcap_set_immediate_mode(pcap_t *p, int mode) {
  return PCAP_ERROR;
}

#ifndef PCAP_TSTAMP_HOST  // < v1.2

int pcap_set_tstamp_type(pcap_t* p, int t) { return -1; }
int pcap_list_tstamp_types(pcap_t* p, int** t) { return 0; }
void pcap_free_tstamp_types(int *tstamp_types) {}
const char* pcap_tstamp_type_val_to_name(int t) {
	return "pcap timestamp types not supported";
}
int pcap_tstamp_type_name_to_val(const char* t) {
	return PCAP_ERROR;
}

#endif  // < v1.2
#endif  // < v1.5

#ifndef PCAP_ERROR_PROMISC_PERM_DENIED
#define PCAP_ERROR_PROMISC_PERM_DENIED -11
#endif

// WinPcap doesn't export a pcap_statustostr, so use the less-specific
// pcap_strerror.  Note that linking against something like cygwin libpcap
// may result is less-specific error messages.
#ifdef WIN32
#define pcap_statustostr pcap_strerror

// WinPcap also doesn't export pcap_can_set_rfmon and pcap_set_rfmon,
// as those are handled by separate libraries (airpcap).
// https://www.winpcap.org/docs/docs_412/html/group__wpcapfunc.html
// Stub out those functions here, returning values that indicate rfmon
// setting is unavailable/unsuccessful.
int pcap_can_set_rfmon(pcap_t *p) {
	return 0;
}

int pcap_set_rfmon(pcap_t *p, int rfmon) {
	return PCAP_ERROR;
}
#endif

// Windows, Macs, and Linux all use different time types.  Joy.
#ifdef WIN32
#define gopacket_time_secs_t long
#define gopacket_time_usecs_t long
#elif __APPLE__
#define gopacket_time_secs_t __darwin_time_t
#define gopacket_time_usecs_t __darwin_suseconds_t
#elif __GLIBC__
#define gopacket_time_secs_t __time_t
#define gopacket_time_usecs_t __suseconds_t
#else
#define gopacket_time_secs_t time_t
#define gopacket_time_usecs_t suseconds_t
#endif
*/
import "C"

import (
	"errors"
	"fmt"
	"io"
	"net"
	"reflect"
	"runtime"
	"strconv"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

const errorBufferSize = 256

// Maximum number of BPF instructions supported (BPF_MAXINSNS),
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
	// cptr is the handle for the actual pcap C object.
	cptr         *C.pcap_t
	blockForever bool
	device       string
	mu           sync.Mutex
	// Since pointers to these objects are passed into a C function, if
	// they're declared locally then the Go compiler thinks they may have
	// escaped into C-land, so it allocates them on the heap.  This causes a
	// huge memory hit, so to handle that we store them here instead.
	pkthdr  *C.struct_pcap_pkthdr
	buf_ptr *C.u_char
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
	Addresses   []InterfaceAddress
	// TODO: add more elements
}

// Datalink describes the datalink
type Datalink struct {
	Name        string
	Description string
}

// InterfaceAddress describes an address associated with an Interface.
// Currently, it's IPv4/6 specific.
type InterfaceAddress struct {
	IP      net.IP
	Netmask net.IPMask // Netmask may be nil if we were unable to retrieve it.
	// TODO: add broadcast + PtP dst ?
}

// BPF is a compiled filter program, useful for offline packet matching.
type BPF struct {
	orig string
	bpf  _Ctype_struct_bpf_program // takes a finalizer, not overriden by outsiders
}

// BPFInstruction is a byte encoded structure holding a BPF instruction
type BPFInstruction struct {
	Code uint16
	Jt   uint8
	Jf   uint8
	K    uint32
}

// BlockForever, when passed into OpenLive/SetTimeout, causes it to block forever
// waiting for packets, while still returning incoming packets to userland relatively
// quickly.
const BlockForever = -time.Millisecond * 10

func timeoutMillis(timeout time.Duration) C.int {
	// Flip sign if necessary.  See package docs on timeout for reasoning behind this.
	if timeout < 0 {
		timeout *= -1
	}
	// Round up
	if timeout != 0 && timeout < time.Millisecond {
		timeout = time.Millisecond
	}
	return C.int(timeout / time.Millisecond)
}

// OpenLive opens a device and returns a *Handle.
// It takes as arguments the name of the device ("eth0"), the maximum size to
// read for each packet (snaplen), whether to put the interface in promiscuous
// mode, and a timeout.
//
// See the package documentation for important details regarding 'timeout'.
func OpenLive(device string, snaplen int32, promisc bool, timeout time.Duration) (handle *Handle, _ error) {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	var pro C.int
	if promisc {
		pro = 1
	}
	p := &Handle{}
	p.blockForever = timeout < 0
	p.device = device

	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))

	p.cptr = C.pcap_open_live(dev, C.int(snaplen), pro, timeoutMillis(timeout), buf)
	if p.cptr == nil {
		return nil, errors.New(C.GoString(buf))
	}
	return p, nil
}

// OpenOffline opens a file and returns its contents as a *Handle.
func OpenOffline(file string) (handle *Handle, err error) {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	cf := C.CString(file)
	defer C.free(unsafe.Pointer(cf))

	cptr := C.pcap_open_offline(cf, buf)
	if cptr == nil {
		return nil, errors.New(C.GoString(buf))
	}
	return &Handle{cptr: cptr}, nil
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

const (
	NextErrorOk             NextError = 1
	NextErrorTimeoutExpired NextError = 0
	NextErrorReadError      NextError = -1
	// NextErrorNoMorePackets is returned when reading from a file (OpenOffline) and
	// EOF is reached.  When this happens, Next() returns io.EOF instead of this.
	NextErrorNoMorePackets NextError = -2
	NextErrorNotActivated  NextError = -3
)

// NextError returns the next packet read from the pcap handle, along with an error
// code associated with that packet.  If the packet is read successfully, the
// returned error is nil.
func (p *Handle) ReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	p.mu.Lock()
	err = p.getNextBufPtrLocked(&ci)
	if err == nil {
		data = C.GoBytes(unsafe.Pointer(p.buf_ptr), C.int(ci.CaptureLength))
	}
	p.mu.Unlock()
	return
}

type activateError C.int

const (
	aeNoError      = 0
	aeActivated    = C.PCAP_ERROR_ACTIVATED
	aePromisc      = C.PCAP_WARNING_PROMISC_NOTSUP
	aeNoSuchDevice = C.PCAP_ERROR_NO_SUCH_DEVICE
	aeDenied       = C.PCAP_ERROR_PERM_DENIED
	aeNotUp        = C.PCAP_ERROR_IFACE_NOT_UP
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
	default:
		return fmt.Sprintf("unknown activated error: %d", a)
	}
}

// getNextBufPtrLocked is shared code for ReadPacketData and
// ZeroCopyReadPacketData.
func (p *Handle) getNextBufPtrLocked(ci *gopacket.CaptureInfo) error {
	if p.cptr == nil {
		return io.EOF
	}
	var result NextError
	for {
		result = NextError(C.pcap_next_ex(p.cptr, &p.pkthdr, &p.buf_ptr))
		if p.blockForever && result == NextErrorTimeoutExpired {
			continue
		}
		break
	}
	if result != NextErrorOk {
		if result == NextErrorNoMorePackets {
			return io.EOF
		} else {
			return result
		}
	}
	ci.Timestamp = time.Unix(int64(p.pkthdr.ts.tv_sec),
		int64(p.pkthdr.ts.tv_usec)*1000) // convert micros to nanos
	ci.CaptureLength = int(p.pkthdr.caplen)
	ci.Length = int(p.pkthdr.len)
	return nil
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
		slice.Data = uintptr(unsafe.Pointer(p.buf_ptr))
		slice.Len = ci.CaptureLength
		slice.Cap = ci.CaptureLength
	}
	p.mu.Unlock()
	return
}

// Close closes the underlying pcap handle.
func (p *Handle) Close() {
	p.mu.Lock()
	if p.cptr == nil {
		return
	}
	C.pcap_close(p.cptr)
	p.cptr = nil
	p.mu.Unlock()
}

// Error returns the current error associated with a pcap handle (pcap_geterr).
func (p *Handle) Error() error {
	return errors.New(C.GoString(C.pcap_geterr(p.cptr)))
}

// Stats returns statistics on the underlying pcap handle.
func (p *Handle) Stats() (stat *Stats, err error) {
	var cstats _Ctype_struct_pcap_stat
	if -1 == C.pcap_stats(p.cptr, &cstats) {
		return nil, p.Error()
	}
	return &Stats{
		PacketsReceived:  int(cstats.ps_recv),
		PacketsDropped:   int(cstats.ps_drop),
		PacketsIfDropped: int(cstats.ps_ifdrop),
	}, nil
}

// Obtains a list of all possible data link types supported for an interface.
func (p *Handle) ListDataLinks() (datalinks []Datalink, err error) {
	var dlt_buf *C.int

	n := int(C.pcap_list_datalinks(p.cptr, &dlt_buf))
	if -1 == n {
		return nil, p.Error()
	}

	defer C.pcap_free_datalinks(dlt_buf)

	datalinks = make([]Datalink, n)

	dltArray := (*[100]C.int)(unsafe.Pointer(dlt_buf))

	for i := 0; i < n; i++ {
		expr := C.pcap_datalink_val_to_name((*dltArray)[i])
		datalinks[i].Name = C.GoString(expr)

		expr = C.pcap_datalink_val_to_description((*dltArray)[i])
		datalinks[i].Description = C.GoString(expr)
	}

	return datalinks, nil
}

func (p *Handle) compileBPFFilter(expr string) (_Ctype_struct_bpf_program, error) {
	errorBuf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(errorBuf))

	var netp uint32
	var maskp uint32

	// Only do the lookup on network interfaces.
	// No device indicates we're handling a pcap file.
	if len(p.device) > 0 {
		dev := C.CString(p.device)
		defer C.free(unsafe.Pointer(dev))
		if -1 == C.pcap_lookupnet(
			dev,
			(*C.bpf_u_int32)(unsafe.Pointer(&netp)),
			(*C.bpf_u_int32)(unsafe.Pointer(&maskp)),
			errorBuf,
		) {
			// We can't lookup the network, but that could be because the interface
			// doesn't have an IPv4.
		}
	}

	var bpf _Ctype_struct_bpf_program
	cexpr := C.CString(expr)
	defer C.free(unsafe.Pointer(cexpr))

	if -1 == C.pcap_compile(p.cptr, &bpf, cexpr, 1, C.bpf_u_int32(maskp)) {
		return bpf, p.Error()
	}

	return bpf, nil
}

// CompileBPFFilter compiles and returns a BPF filter for the pcap handle.
func (p *Handle) CompileBPFFilter(expr string) ([]BPFInstruction, error) {
	bpf, err := p.compileBPFFilter(expr)
	if err != nil {
		return nil, err
	}

	bpfInsn := (*[bpfInstructionBufferSize]_Ctype_struct_bpf_insn)(unsafe.Pointer(bpf.bf_insns))[0:bpf.bf_len:bpf.bf_len]
	bpfInstruction := make([]BPFInstruction, len(bpfInsn), len(bpfInsn))

	for i, v := range bpfInsn {
		bpfInstruction[i].Code = uint16(v.code)
		bpfInstruction[i].Jt = uint8(v.jt)
		bpfInstruction[i].Jf = uint8(v.jf)
		bpfInstruction[i].K = uint32(v.k)
	}

	C.pcap_freecode(&bpf)

	return bpfInstruction, nil
}

// SetBPFFilter compiles and sets a BPF filter for the pcap handle.
func (p *Handle) SetBPFFilter(expr string) (err error) {
	bpf, err := p.compileBPFFilter(expr)
	if err != nil {
		return err
	}

	if -1 == C.pcap_setfilter(p.cptr, &bpf) {
		C.pcap_freecode(&bpf)
		return p.Error()
	}

	C.pcap_freecode(&bpf)

	return nil
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

	if -1 == C.pcap_setfilter(p.cptr, &bpf) {
		C.pcap_freecode(&bpf)
		return p.Error()
	}

	C.pcap_freecode(&bpf)

	return nil
}
func bpfInstructionFilter(bpfInstructions []BPFInstruction) (bpf _Ctype_struct_bpf_program, err error) {
	if len(bpfInstructions) < 1 {
		return bpf, errors.New("bpfInstructions must not be empty")
	}

	if len(bpfInstructions) > MaxBpfInstructions {
		return bpf, fmt.Errorf("bpfInstructions must not be larger than %d", MaxBpfInstructions)
	}

	bpf.bf_len = C.u_int(len(bpfInstructions))
	cbpfInsns := C.calloc(C.size_t(len(bpfInstructions)), C.size_t(unsafe.Sizeof(bpfInstructions[0])))

	copy((*[bpfInstructionBufferSize]BPFInstruction)(cbpfInsns)[0:len(bpfInstructions)], bpfInstructions)
	bpf.bf_insns = (*_Ctype_struct_bpf_insn)(cbpfInsns)

	return
}

// NewBPF compiles the given string into a new filter program.
//
// BPF filters need to be created from activated handles, because they need to
// know the underlying link type to correctly compile their offsets.
func (p *Handle) NewBPF(expr string) (*BPF, error) {
	bpf := &BPF{orig: expr}
	cexpr := C.CString(expr)
	defer C.free(unsafe.Pointer(cexpr))

	if C.pcap_compile(p.cptr, &bpf.bpf, cexpr /* optimize */, 1, C.PCAP_NETMASK_UNKNOWN) != 0 {
		return nil, p.Error()
	}

	runtime.SetFinalizer(bpf, destroyBPF)
	return bpf, nil
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
	C.pcap_freecode(&bpf.bpf)
}

// String returns the original string this BPF filter was compiled from.
func (b *BPF) String() string {
	return b.orig
}

// Matches returns true if the given packet data matches this filter.
func (b *BPF) Matches(ci gopacket.CaptureInfo, data []byte) bool {
	var hdr C.struct_pcap_pkthdr
	hdr.ts.tv_sec = C.gopacket_time_secs_t(ci.Timestamp.Unix())
	hdr.ts.tv_usec = C.gopacket_time_usecs_t(ci.Timestamp.Nanosecond() / 1000)
	hdr.caplen = C.bpf_u_int32(len(data)) // Trust actual length over ci.Length.
	hdr.len = C.bpf_u_int32(ci.Length)
	dataptr := (*C.u_char)(unsafe.Pointer(&data[0]))
	return C.pcap_offline_filter(&b.bpf, &hdr, dataptr) != 0
}

// Version returns pcap_lib_version.
func Version() string {
	return C.GoString(C.pcap_lib_version())
}

// LinkType returns pcap_datalink, as a layers.LinkType.
func (p *Handle) LinkType() layers.LinkType {
	return layers.LinkType(C.pcap_datalink(p.cptr))
}

// SetLinkType calls pcap_set_datalink on the pcap handle.
func (p *Handle) SetLinkType(dlt layers.LinkType) error {
	if -1 == C.pcap_set_datalink(p.cptr, C.int(dlt)) {
		return p.Error()
	}
	return nil
}

// FindAllDevs attempts to enumerate all interfaces on the current machine.
func FindAllDevs() (ifs []Interface, err error) {
	var buf *C.char
	buf = (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	var alldevsp *C.pcap_if_t

	if -1 == C.pcap_findalldevs((**C.pcap_if_t)(&alldevsp), buf) {
		return nil, errors.New(C.GoString(buf))
	}
	defer C.pcap_freealldevs((*C.pcap_if_t)(alldevsp))
	dev := alldevsp
	var i uint32
	for i = 0; dev != nil; dev = (*C.pcap_if_t)(dev.next) {
		i++
	}
	ifs = make([]Interface, i)
	dev = alldevsp
	for j := uint32(0); dev != nil; dev = (*C.pcap_if_t)(dev.next) {
		var iface Interface
		iface.Name = C.GoString(dev.name)
		iface.Description = C.GoString(dev.description)
		iface.Addresses = findalladdresses(dev.addresses)
		// TODO: add more elements
		ifs[j] = iface
		j++
	}
	return
}

func findalladdresses(addresses *_Ctype_struct_pcap_addr) (retval []InterfaceAddress) {
	// TODO - make it support more than IPv4 and IPv6?
	retval = make([]InterfaceAddress, 0, 1)
	for curaddr := addresses; curaddr != nil; curaddr = (*_Ctype_struct_pcap_addr)(curaddr.next) {
		// Strangely, it appears that in some cases, we get a pcap address back from
		// pcap_findalldevs with a nil .addr.  It appears that we can skip over
		// these.
		if curaddr.addr == nil {
			continue
		}
		var a InterfaceAddress
		var err error
		if a.IP, err = sockaddr_to_IP((*syscall.RawSockaddr)(unsafe.Pointer(curaddr.addr))); err != nil {
			continue
		}
		// To be safe, we'll also check for netmask.
		if curaddr.netmask == nil {
			continue
		}
		if a.Netmask, err = sockaddr_to_IP((*syscall.RawSockaddr)(unsafe.Pointer(curaddr.netmask))); err != nil {
			// If we got an IP address but we can't get a netmask, just return the IP
			// address.
			a.Netmask = nil
		}
		retval = append(retval, a)
	}
	return
}

func sockaddr_to_IP(rsa *syscall.RawSockaddr) (IP []byte, err error) {
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
	if -1 == C.pcap_sendpacket(p.cptr, (*C.u_char)(&data[0]), (C.int)(len(data))) {
		err = p.Error()
	}
	return
}

// Direction is used by Handle.SetDirection.
type Direction uint8

const (
	DirectionIn    Direction = C.PCAP_D_IN
	DirectionOut   Direction = C.PCAP_D_OUT
	DirectionInOut Direction = C.PCAP_D_INOUT
)

// SetDirection sets the direction for which packets will be captured.
func (p *Handle) SetDirection(direction Direction) error {
	if direction != DirectionIn && direction != DirectionOut && direction != DirectionInOut {
		return fmt.Errorf("Invalid direction: %v", direction)
	}
	if status := C.pcap_setdirection(p.cptr, (C.pcap_direction_t)(direction)); status < 0 {
		return statusError(status)
	}
	return nil
}

// TimestampSource tells PCAP which type of timestamp to use for packets.
type TimestampSource C.int

// String returns the timestamp type as a human-readable string.
func (t TimestampSource) String() string {
	return C.GoString(C.pcap_tstamp_type_val_to_name(C.int(t)))
}

// TimestampSourceFromString translates a string into a timestamp type, case
// insensitive.
func TimestampSourceFromString(s string) (TimestampSource, error) {
	t := C.pcap_tstamp_type_name_to_val(C.CString(s))
	if t < 0 {
		return 0, statusError(t)
	}
	return TimestampSource(t), nil
}

func statusError(status C.int) error {
	return errors.New(C.GoString(C.pcap_statustostr(status)))
}

// InactiveHandle allows you to call pre-pcap_activate functions on your pcap
// handle to set it up just the way you'd like.
type InactiveHandle struct {
	// cptr is the handle for the actual pcap C object.
	cptr         *C.pcap_t
	device       string
	blockForever bool
}

// Activate activates the handle.  The current InactiveHandle becomes invalid
// and all future function calls on it will fail.
func (p *InactiveHandle) Activate() (*Handle, error) {
	err := activateError(C.pcap_activate(p.cptr))
	if err != aeNoError {
		return nil, err
	}
	h := &Handle{cptr: p.cptr, device: p.device, blockForever: p.blockForever}
	p.cptr = nil
	return h, nil
}

// CleanUp cleans up any stuff left over from a successful or failed building
// of a handle.
func (p *InactiveHandle) CleanUp() {
	if p.cptr != nil {
		C.pcap_close(p.cptr)
	}
}

// NewInactiveHandle creates a new InactiveHandle, which wraps an un-activated PCAP handle.
// Callers of NewInactiveHandle should immediately defer 'CleanUp', as in:
//   inactive := NewInactiveHandle("eth0")
//   defer inactive.CleanUp()
func NewInactiveHandle(device string) (*InactiveHandle, error) {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))

	// This copies a bunch of the pcap_open_live implementation from pcap.c:
	cptr := C.pcap_create(dev, buf)
	if cptr == nil {
		return nil, errors.New(C.GoString(buf))
	}
	return &InactiveHandle{cptr: cptr, device: device}, nil
}

// SetSnapLen sets the snap length (max bytes per packet to capture).
func (p *InactiveHandle) SetSnapLen(snaplen int) error {
	if status := C.pcap_set_snaplen(p.cptr, C.int(snaplen)); status < 0 {
		return statusError(status)
	}
	return nil
}

// SetPromisc sets the handle to either be promiscuous (capture packets
// unrelated to this host) or not.
func (p *InactiveHandle) SetPromisc(promisc bool) error {
	var pro C.int
	if promisc {
		pro = 1
	}
	if status := C.pcap_set_promisc(p.cptr, pro); status < 0 {
		return statusError(status)
	}
	return nil
}

// SetTimeout sets the read timeout for the handle.
//
// See the package documentation for important details regarding 'timeout'.
func (p *InactiveHandle) SetTimeout(timeout time.Duration) error {
	p.blockForever = timeout < 0
	if status := C.pcap_set_timeout(p.cptr, timeoutMillis(timeout)); status < 0 {
		return statusError(status)
	}
	return nil
}

// SupportedTimestamps returns a list of supported timstamp types for this
// handle.
func (p *InactiveHandle) SupportedTimestamps() (out []TimestampSource) {
	var types *C.int
	n := int(C.pcap_list_tstamp_types(p.cptr, &types))
	defer C.pcap_free_tstamp_types(types)
	typesArray := (*[100]C.int)(unsafe.Pointer(types))
	for i := 0; i < n; i++ {
		out = append(out, TimestampSource((*typesArray)[i]))
	}
	return
}

// SetTimestampSource sets the type of timestamp generator PCAP uses when
// attaching timestamps to packets.
func (p *InactiveHandle) SetTimestampSource(t TimestampSource) error {
	if status := C.pcap_set_tstamp_type(p.cptr, C.int(t)); status < 0 {
		return statusError(status)
	}
	return nil
}

// CannotSetRFMon is returned by SetRFMon if the handle does not allow
// setting RFMon because pcap_can_set_rfmon returns 0.
var CannotSetRFMon = errors.New("Cannot set rfmon for this handle")

// SetRFMon turns on radio monitoring mode, similar to promiscuous mode but for
// wireless networks.  If this mode is enabled, the interface will not need to
// associate with an access point before it can receive traffic.
func (p *InactiveHandle) SetRFMon(monitor bool) error {
	var mon C.int
	if monitor {
		mon = 1
	}
	switch canset := C.pcap_can_set_rfmon(p.cptr); canset {
	case 0:
		return CannotSetRFMon
	case 1:
		// success
	default:
		return statusError(canset)
	}
	if status := C.pcap_set_rfmon(p.cptr, mon); status != 0 {
		return statusError(status)
	}
	return nil
}

// SetBufferSize sets the buffer size (in bytes) of the handle.
func (p *InactiveHandle) SetBufferSize(bufferSize int) error {
	if status := C.pcap_set_buffer_size(p.cptr, C.int(bufferSize)); status < 0 {
		return statusError(status)
	}
	return nil
}

// SetImmediateMode sets (or unsets) the immediate mode of the
// handle. In immediate mode, packets are delivered to the application
// as soon as they arrive.  In other words, this overrides SetTimeout.
func (p *InactiveHandle) SetImmediateMode(mode bool) error {
	var md C.int
	if mode {
		md = 1
	}
	if status := C.pcap_set_immediate_mode(p.cptr, md); status < 0 {
		return statusError(status)
	}
	return nil
}
