// Copyright 2012 Google, Inc. All rights reserved.
// Copyright 2009-2011 Andreas Krennmair. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
//
// +build !windows

package pcap

import (
	"errors"
	"os"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"github.com/google/gopacket"

	"github.com/google/gopacket/layers"
)

/*
#cgo solaris LDFLAGS: -L /opt/local/lib -lpcap
#cgo linux LDFLAGS: -lpcap
#cgo dragonfly LDFLAGS: -lpcap
#cgo freebsd LDFLAGS: -lpcap
#cgo openbsd LDFLAGS: -lpcap
#cgo netbsd LDFLAGS: -lpcap
#cgo darwin LDFLAGS: -lpcap
#include <stdlib.h>
#include <pcap.h>
#include <stdint.h>

// Some old versions of pcap don't define this constant.
#ifndef PCAP_NETMASK_UNKNOWN
#define PCAP_NETMASK_UNKNOWN 0xffffffff
#endif

// libpcap doesn't actually export its version in a #define-guardable way,
// so we have to use other defined things to differentiate versions.
// We assume at least libpcap v1.1 at the moment.
// See http://upstream-tracker.org/versions/libpcap.html

#ifndef PCAP_ERROR_TSTAMP_PRECISION_NOTSUP  // < v1.5
#define PCAP_ERROR_TSTAMP_PRECISION_NOTSUP -12

int pcap_set_immediate_mode(pcap_t *p, int mode) {
  return PCAP_ERROR;
}

//  libpcap version < v1.5 doesn't have timestamp precision (everything is microsecond)
//
//  This means *_tstamp_* functions and macros are missing. Therefore, we emulate these
//  functions here and pretend the setting the precision works. This is actually the way
//  the pcap_open_offline_with_tstamp_precision works, because it doesn't return an error
//  if it was not possible to set the precision, which depends on support by the given file.
//  => The rest of the functions always pretend as if they could set nano precision and
//  verify the actual precision with pcap_get_tstamp_precision, which is emulated for <v1.5
//  to always return micro resolution.

#define PCAP_TSTAMP_PRECISION_MICRO	0
#define PCAP_TSTAMP_PRECISION_NANO	1

pcap_t *pcap_open_offline_with_tstamp_precision(const char *fname, u_int precision,
  char *errbuf) {
  return pcap_open_offline(fname, errbuf);
}

pcap_t *pcap_fopen_offline_with_tstamp_precision(FILE *fp, u_int precision,
  char *errbuf) {
  return pcap_fopen_offline(fp, errbuf);
}

int pcap_set_tstamp_precision(pcap_t *p, int tstamp_precision) {
  if (tstamp_precision == PCAP_TSTAMP_PRECISION_MICRO)
    return 0;
  return PCAP_ERROR_TSTAMP_PRECISION_NOTSUP;
}

int pcap_get_tstamp_precision(pcap_t *p) {
  return PCAP_TSTAMP_PRECISION_MICRO;
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

// Windows, Macs, and Linux all use different time types.  Joy.
#ifdef __APPLE__
#define gopacket_time_secs_t __darwin_time_t
#define gopacket_time_usecs_t __darwin_suseconds_t
#elif __ANDROID__
#define gopacket_time_secs_t __kernel_time_t
#define gopacket_time_usecs_t __kernel_suseconds_t
#elif __GLIBC__
#define gopacket_time_secs_t __time_t
#define gopacket_time_usecs_t __suseconds_t
#else  // Some form of linux/bsd/etc...
#include <sys/param.h>
#ifdef __OpenBSD__
#define gopacket_time_secs_t u_int32_t
#define gopacket_time_usecs_t u_int32_t
#else
#define gopacket_time_secs_t time_t
#define gopacket_time_usecs_t suseconds_t
#endif
#endif

// The things we do to avoid pointers escaping to the heap...
// According to https://github.com/the-tcpdump-group/libpcap/blob/1131a7c26c6f4d4772e4a2beeaf7212f4dea74ac/pcap.c#L398-L406 ,
// the return value of pcap_next_ex could be greater than 1 for success.
// Let's just make it 1 if it comes bigger than 1.
int pcap_next_ex_escaping(pcap_t *p, uintptr_t pkt_hdr, uintptr_t pkt_data) {
  int ex = pcap_next_ex(p, (struct pcap_pkthdr**)(pkt_hdr), (const u_char**)(pkt_data));
  if (ex > 1) {
    ex = 1;
  }
  return ex;
}

int pcap_offline_filter_escaping(struct bpf_program *fp, uintptr_t pkt_hdr, uintptr_t pkt) {
	return pcap_offline_filter(fp, (struct pcap_pkthdr*)(pkt_hdr), (const u_char*)(pkt));
}

// pcap_wait returns when the next packet is available or the timeout expires.
// Since it uses pcap_get_selectable_fd, it will not work in Windows.
int pcap_wait(pcap_t *p, int usec) {
	fd_set fds;
	int fd;
	struct timeval tv;

	fd = pcap_get_selectable_fd(p);
	if(fd < 0) {
		return fd;
	}

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	tv.tv_sec = 0;
	tv.tv_usec = usec;

	if(usec != 0) {
		return select(fd+1, &fds, NULL, NULL, &tv);
	}

	// block indefinitely if no timeout provided
	return select(fd+1, &fds, NULL, NULL, NULL);
}

*/
import "C"

const errorBufferSize = C.PCAP_ERRBUF_SIZE

const (
	pcapErrorNotActivated    = C.PCAP_ERROR_NOT_ACTIVATED
	pcapErrorActivated       = C.PCAP_ERROR_ACTIVATED
	pcapWarningPromisc       = C.PCAP_WARNING_PROMISC_NOTSUP
	pcapErrorNoSuchDevice    = C.PCAP_ERROR_NO_SUCH_DEVICE
	pcapErrorDenied          = C.PCAP_ERROR_PERM_DENIED
	pcapErrorNotUp           = C.PCAP_ERROR_IFACE_NOT_UP
	pcapWarning              = C.PCAP_WARNING
	pcapDIN                  = C.PCAP_D_IN
	pcapDOUT                 = C.PCAP_D_OUT
	pcapDINOUT               = C.PCAP_D_INOUT
	pcapNetmaskUnknown       = C.PCAP_NETMASK_UNKNOWN
	pcapTstampPrecisionMicro = C.PCAP_TSTAMP_PRECISION_MICRO
	pcapTstampPrecisionNano  = C.PCAP_TSTAMP_PRECISION_NANO
)

type pcapPkthdr C.struct_pcap_pkthdr
type pcapTPtr *C.struct_pcap
type pcapBpfProgram C.struct_bpf_program

func (h *pcapPkthdr) getSec() int64 {
	return int64(h.ts.tv_sec)
}

func (h *pcapPkthdr) getUsec() int64 {
	return int64(h.ts.tv_usec)
}

func (h *pcapPkthdr) getLen() int {
	return int(h.len)
}

func (h *pcapPkthdr) getCaplen() int {
	return int(h.caplen)
}

func pcapGetTstampPrecision(cptr pcapTPtr) int {
	return int(C.pcap_get_tstamp_precision(cptr))
}

func pcapSetTstampPrecision(cptr pcapTPtr, precision int) error {
	ret := C.pcap_set_tstamp_precision(cptr, C.int(precision))
	if ret < 0 {
		return errors.New(C.GoString(C.pcap_geterr(cptr)))
	}
	return nil
}

func statusError(status C.int) error {
	return errors.New(C.GoString(C.pcap_statustostr(status)))
}

func pcapOpenLive(device string, snaplen int, pro int, timeout int) (*Handle, error) {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))

	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))

	cptr := C.pcap_open_live(dev, C.int(snaplen), C.int(pro), C.int(timeout), buf)
	if cptr == nil {
		return nil, errors.New(C.GoString(buf))
	}
	return &Handle{cptr: cptr}, nil
}

func openOffline(file string) (handle *Handle, err error) {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	cf := C.CString(file)
	defer C.free(unsafe.Pointer(cf))

	cptr := C.pcap_open_offline_with_tstamp_precision(cf, C.PCAP_TSTAMP_PRECISION_NANO, buf)
	if cptr == nil {
		return nil, errors.New(C.GoString(buf))
	}
	return &Handle{cptr: cptr}, nil
}

func (p *Handle) pcapClose() {
	if p.cptr != nil {
		C.pcap_close(p.cptr)
	}
	p.cptr = nil
}

func (p *Handle) pcapGeterr() error {
	return errors.New(C.GoString(C.pcap_geterr(p.cptr)))
}

func (p *Handle) pcapStats() (*Stats, error) {
	var cstats C.struct_pcap_stat
	if C.pcap_stats(p.cptr, &cstats) < 0 {
		return nil, p.pcapGeterr()
	}
	return &Stats{
		PacketsReceived:  int(cstats.ps_recv),
		PacketsDropped:   int(cstats.ps_drop),
		PacketsIfDropped: int(cstats.ps_ifdrop),
	}, nil
}

// for libpcap < 1.8 pcap_compile is NOT thread-safe, so protect it.
var pcapCompileMu sync.Mutex

func (p *Handle) pcapCompile(expr string, maskp uint32) (pcapBpfProgram, error) {
	var bpf pcapBpfProgram
	cexpr := C.CString(expr)
	defer C.free(unsafe.Pointer(cexpr))

	pcapCompileMu.Lock()
	defer pcapCompileMu.Unlock()
	if C.pcap_compile(p.cptr, (*C.struct_bpf_program)(&bpf), cexpr, 1, C.bpf_u_int32(maskp)) < 0 {
		return bpf, p.pcapGeterr()
	}
	return bpf, nil
}

func (p pcapBpfProgram) free() {
	C.pcap_freecode((*C.struct_bpf_program)(&p))
}

func (p pcapBpfProgram) toBPFInstruction() []BPFInstruction {
	bpfInsn := (*[bpfInstructionBufferSize]C.struct_bpf_insn)(unsafe.Pointer(p.bf_insns))[0:p.bf_len:p.bf_len]
	bpfInstruction := make([]BPFInstruction, len(bpfInsn), len(bpfInsn))

	for i, v := range bpfInsn {
		bpfInstruction[i].Code = uint16(v.code)
		bpfInstruction[i].Jt = uint8(v.jt)
		bpfInstruction[i].Jf = uint8(v.jf)
		bpfInstruction[i].K = uint32(v.k)
	}
	return bpfInstruction
}

func pcapBpfProgramFromInstructions(bpfInstructions []BPFInstruction) pcapBpfProgram {
	var bpf pcapBpfProgram
	bpf.bf_len = C.u_int(len(bpfInstructions))
	cbpfInsns := C.calloc(C.size_t(len(bpfInstructions)), C.size_t(unsafe.Sizeof(bpfInstructions[0])))
	gbpfInsns := (*[bpfInstructionBufferSize]C.struct_bpf_insn)(cbpfInsns)

	for i, v := range bpfInstructions {
		gbpfInsns[i].code = C.u_short(v.Code)
		gbpfInsns[i].jt = C.u_char(v.Jt)
		gbpfInsns[i].jf = C.u_char(v.Jf)
		gbpfInsns[i].k = C.bpf_u_int32(v.K)
	}

	bpf.bf_insns = (*C.struct_bpf_insn)(cbpfInsns)
	return bpf
}

func pcapLookupnet(device string) (netp, maskp uint32, err error) {
	errorBuf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(errorBuf))
	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))
	if C.pcap_lookupnet(
		dev,
		(*C.bpf_u_int32)(unsafe.Pointer(&netp)),
		(*C.bpf_u_int32)(unsafe.Pointer(&maskp)),
		errorBuf,
	) < 0 {
		return 0, 0, errors.New(C.GoString(errorBuf))
		// We can't lookup the network, but that could be because the interface
		// doesn't have an IPv4.
	}
	return
}

func (b *BPF) pcapOfflineFilter(ci gopacket.CaptureInfo, data []byte) bool {
	hdr := (*C.struct_pcap_pkthdr)(&b.hdr)
	hdr.ts.tv_sec = C.gopacket_time_secs_t(ci.Timestamp.Unix())
	hdr.ts.tv_usec = C.gopacket_time_usecs_t(ci.Timestamp.Nanosecond() / 1000)
	hdr.caplen = C.bpf_u_int32(len(data)) // Trust actual length over ci.Length.
	hdr.len = C.bpf_u_int32(ci.Length)
	dataptr := (*C.u_char)(unsafe.Pointer(&data[0]))
	return C.pcap_offline_filter_escaping((*C.struct_bpf_program)(&b.bpf),
		C.uintptr_t(uintptr(unsafe.Pointer(hdr))),
		C.uintptr_t(uintptr(unsafe.Pointer(dataptr)))) != 0
}

func (p *Handle) pcapSetfilter(bpf pcapBpfProgram) error {
	if C.pcap_setfilter(p.cptr, (*C.struct_bpf_program)(&bpf)) < 0 {
		return p.pcapGeterr()
	}
	return nil
}

func (p *Handle) pcapListDatalinks() (datalinks []Datalink, err error) {
	var dltbuf *C.int

	n := int(C.pcap_list_datalinks(p.cptr, &dltbuf))
	if n < 0 {
		return nil, p.pcapGeterr()
	}

	defer C.pcap_free_datalinks(dltbuf)

	datalinks = make([]Datalink, n)

	dltArray := (*[1 << 28]C.int)(unsafe.Pointer(dltbuf))

	for i := 0; i < n; i++ {
		datalinks[i].Name = pcapDatalinkValToName(int((*dltArray)[i]))
		datalinks[i].Description = pcapDatalinkValToDescription(int((*dltArray)[i]))
	}

	return datalinks, nil
}

func pcapOpenDead(linkType layers.LinkType, captureLength int) (*Handle, error) {
	cptr := C.pcap_open_dead(C.int(linkType), C.int(captureLength))
	if cptr == nil {
		return nil, errors.New("error opening dead capture")
	}

	return &Handle{cptr: cptr}, nil
}

func (p *Handle) pcapNextPacketEx() NextError {
	// This horrible magic allows us to pass a ptr-to-ptr to pcap_next_ex
	// without causing that ptr-to-ptr to itself be allocated on the heap.
	// Since Handle itself survives through the duration of the pcap_next_ex
	// call, this should be perfectly safe for GC stuff, etc.

	return NextError(C.pcap_next_ex_escaping(p.cptr, C.uintptr_t(uintptr(unsafe.Pointer(&p.pkthdr))), C.uintptr_t(uintptr(unsafe.Pointer(&p.bufptr)))))
}

func (p *Handle) pcapDatalink() layers.LinkType {
	return layers.LinkType(C.pcap_datalink(p.cptr))
}

func (p *Handle) pcapSetDatalink(dlt layers.LinkType) error {
	if C.pcap_set_datalink(p.cptr, C.int(dlt)) < 0 {
		return p.pcapGeterr()
	}
	return nil
}

func pcapDatalinkValToName(dlt int) string {
	return C.GoString(C.pcap_datalink_val_to_name(C.int(dlt)))
}

func pcapDatalinkValToDescription(dlt int) string {
	return C.GoString(C.pcap_datalink_val_to_description(C.int(dlt)))
}

func pcapDatalinkNameToVal(name string) int {
	cptr := C.CString(name)
	defer C.free(unsafe.Pointer(cptr))
	return int(C.pcap_datalink_name_to_val(cptr))
}

func pcapLibVersion() string {
	return C.GoString(C.pcap_lib_version())
}

func (p *Handle) isOpen() bool {
	return p.cptr != nil
}

type pcapDevices struct {
	all, cur *C.pcap_if_t
}

func (p pcapDevices) free() {
	C.pcap_freealldevs((*C.pcap_if_t)(p.all))
}

func (p *pcapDevices) next() bool {
	if p.cur == nil {
		p.cur = p.all
		if p.cur == nil {
			return false
		}
		return true
	}
	if p.cur.next == nil {
		return false
	}
	p.cur = p.cur.next
	return true
}

func (p pcapDevices) name() string {
	return C.GoString(p.cur.name)
}

func (p pcapDevices) description() string {
	return C.GoString(p.cur.description)
}

func (p pcapDevices) flags() uint32 {
	return uint32(p.cur.flags)
}

type pcapAddresses struct {
	all, cur *C.pcap_addr_t
}

func (p *pcapAddresses) next() bool {
	if p.cur == nil {
		p.cur = p.all
		if p.cur == nil {
			return false
		}
		return true
	}
	if p.cur.next == nil {
		return false
	}
	p.cur = p.cur.next
	return true
}

func (p pcapAddresses) addr() *syscall.RawSockaddr {
	return (*syscall.RawSockaddr)(unsafe.Pointer(p.cur.addr))
}

func (p pcapAddresses) netmask() *syscall.RawSockaddr {
	return (*syscall.RawSockaddr)(unsafe.Pointer(p.cur.netmask))
}

func (p pcapAddresses) broadaddr() *syscall.RawSockaddr {
	return (*syscall.RawSockaddr)(unsafe.Pointer(p.cur.broadaddr))
}

func (p pcapAddresses) dstaddr() *syscall.RawSockaddr {
	return (*syscall.RawSockaddr)(unsafe.Pointer(p.cur.dstaddr))
}

func (p pcapDevices) addresses() pcapAddresses {
	return pcapAddresses{all: p.cur.addresses}
}

func pcapFindAllDevs() (pcapDevices, error) {
	var buf *C.char
	buf = (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	var alldevsp pcapDevices

	if C.pcap_findalldevs((**C.pcap_if_t)(&alldevsp.all), buf) < 0 {
		return pcapDevices{}, errors.New(C.GoString(buf))
	}
	return alldevsp, nil
}

func (p *Handle) pcapSendpacket(data []byte) error {
	if C.pcap_sendpacket(p.cptr, (*C.u_char)(&data[0]), (C.int)(len(data))) < 0 {
		return p.pcapGeterr()
	}
	return nil
}

func (p *Handle) pcapSetdirection(direction Direction) error {
	if status := C.pcap_setdirection(p.cptr, (C.pcap_direction_t)(direction)); status < 0 {
		return statusError(status)
	}
	return nil
}

func (p *Handle) pcapSnapshot() int {
	return int(C.pcap_snapshot(p.cptr))
}

func (t TimestampSource) pcapTstampTypeValToName() string {
	return C.GoString(C.pcap_tstamp_type_val_to_name(C.int(t)))
}

func pcapTstampTypeNameToVal(s string) (TimestampSource, error) {
	cs := C.CString(s)
	defer C.free(unsafe.Pointer(cs))
	t := C.pcap_tstamp_type_name_to_val(cs)
	if t < 0 {
		return 0, statusError(t)
	}
	return TimestampSource(t), nil
}

func (p *InactiveHandle) pcapGeterr() error {
	return errors.New(C.GoString(C.pcap_geterr(p.cptr)))
}

func (p *InactiveHandle) pcapActivate() (*Handle, activateError) {
	ret := activateError(C.pcap_activate(p.cptr))
	if ret != aeNoError {
		return nil, ret
	}
	h := &Handle{
		cptr: p.cptr,
	}
	p.cptr = nil
	return h, ret
}

func (p *InactiveHandle) pcapClose() {
	if p.cptr != nil {
		C.pcap_close(p.cptr)
	}
}

func pcapCreate(device string) (*InactiveHandle, error) {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	dev := C.CString(device)
	defer C.free(unsafe.Pointer(dev))

	cptr := C.pcap_create(dev, buf)
	if cptr == nil {
		return nil, errors.New(C.GoString(buf))
	}
	return &InactiveHandle{cptr: cptr}, nil
}

func (p *InactiveHandle) pcapSetSnaplen(snaplen int) error {
	if status := C.pcap_set_snaplen(p.cptr, C.int(snaplen)); status < 0 {
		return statusError(status)
	}
	return nil
}

func (p *InactiveHandle) pcapSetPromisc(promisc bool) error {
	var pro C.int
	if promisc {
		pro = 1
	}
	if status := C.pcap_set_promisc(p.cptr, pro); status < 0 {
		return statusError(status)
	}
	return nil
}

func (p *InactiveHandle) pcapSetTimeout(timeout time.Duration) error {
	if status := C.pcap_set_timeout(p.cptr, C.int(timeoutMillis(timeout))); status < 0 {
		return statusError(status)
	}
	return nil
}

func (p *InactiveHandle) pcapListTstampTypes() (out []TimestampSource) {
	var types *C.int
	n := int(C.pcap_list_tstamp_types(p.cptr, &types))
	if n < 0 {
		return // public interface doesn't have error :(
	}
	defer C.pcap_free_tstamp_types(types)
	typesArray := (*[1 << 28]C.int)(unsafe.Pointer(types))
	for i := 0; i < n; i++ {
		out = append(out, TimestampSource((*typesArray)[i]))
	}
	return
}

func (p *InactiveHandle) pcapSetTstampType(t TimestampSource) error {
	if status := C.pcap_set_tstamp_type(p.cptr, C.int(t)); status < 0 {
		return statusError(status)
	}
	return nil
}

func (p *InactiveHandle) pcapSetRfmon(monitor bool) error {
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

func (p *InactiveHandle) pcapSetBufferSize(bufferSize int) error {
	if status := C.pcap_set_buffer_size(p.cptr, C.int(bufferSize)); status < 0 {
		return statusError(status)
	}
	return nil
}

func (p *InactiveHandle) pcapSetImmediateMode(mode bool) error {
	var md C.int
	if mode {
		md = 1
	}
	if status := C.pcap_set_immediate_mode(p.cptr, md); status < 0 {
		return statusError(status)
	}
	return nil
}

func (p *Handle) setNonBlocking() error {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))

	// Change the device to non-blocking, we'll use pcap_wait to wait until the
	// handle is ready to read.
	if v := C.pcap_setnonblock(p.cptr, 1, buf); v < -1 {
		return errors.New(C.GoString(buf))
	}

	return nil
}

// waitForPacket waits for a packet or for the timeout to expire.
func (p *Handle) waitForPacket() {
	// need to wait less than the read timeout according to pcap documentation.
	// timeoutMillis rounds up to at least one millisecond so we can safely
	// subtract up to a millisecond.
	usec := timeoutMillis(p.timeout) * 1000
	usec -= 100

	C.pcap_wait(p.cptr, C.int(usec))
}

// openOfflineFile returns contents of input file as a *Handle.
func openOfflineFile(file *os.File) (handle *Handle, err error) {
	buf := (*C.char)(C.calloc(errorBufferSize, 1))
	defer C.free(unsafe.Pointer(buf))
	cmode := C.CString("rb")
	defer C.free(unsafe.Pointer(cmode))
	cf := C.fdopen(C.int(file.Fd()), cmode)

	cptr := C.pcap_fopen_offline_with_tstamp_precision(cf, C.PCAP_TSTAMP_PRECISION_NANO, buf)
	if cptr == nil {
		return nil, errors.New(C.GoString(buf))
	}
	return &Handle{cptr: cptr}, nil
}
