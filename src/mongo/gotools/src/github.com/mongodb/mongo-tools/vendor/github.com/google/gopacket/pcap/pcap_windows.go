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
	"os"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

const npcapPath = "\\Npcap"

func initDllPath(kernel32 syscall.Handle) {
	setDllDirectory, err := syscall.GetProcAddress(kernel32, "SetDllDirectoryA")
	if err != nil {
		// we can't do anything since SetDllDirectoryA is missing - fall back to use first wpcap.dll we encounter
		return
	}
	getSystemDirectory, err := syscall.GetProcAddress(kernel32, "GetSystemDirectoryA")
	if err != nil {
		// we can't do anything since SetDllDirectoryA is missing - fall back to use first wpcap.dll we encounter
		return
	}
	buf := make([]byte, 4096)
	r, _, _ := syscall.Syscall(getSystemDirectory, 2, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)), 0)
	if r == 0 || r > 4096-uintptr(len(npcapPath))-1 {
		// we can't do anything since SetDllDirectoryA is missing - fall back to use first wpcap.dll we encounter
		return
	}
	copy(buf[r:], npcapPath)
	_, _, _ = syscall.Syscall(setDllDirectory, 1, uintptr(unsafe.Pointer(&buf[0])), 0, 0)
	// ignore errors here - we just fallback to load wpcap.dll from default locations
}

// loadedDllPath will hold the full pathname of the loaded wpcap.dll after init if possible
var loadedDllPath = "wpcap.dll"

func initLoadedDllPath(kernel32 syscall.Handle) {
	getModuleFileName, err := syscall.GetProcAddress(kernel32, "GetModuleFileNameA")
	if err != nil {
		// we can't get the filename of the loaded module in this case - just leave default of wpcap.dll
		return
	}
	buf := make([]byte, 4096)
	r, _, _ := syscall.Syscall(getModuleFileName, 3, uintptr(wpcapHandle), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	if r == 0 {
		// we can't get the filename of the loaded module in this case - just leave default of wpcap.dll
		return
	}
	loadedDllPath = string(buf[:int(r)])
}

func mustLoad(fun string) uintptr {
	addr, err := syscall.GetProcAddress(wpcapHandle, fun)
	if err != nil {
		panic(fmt.Sprintf("Couldn't load function %s from %s", fun, loadedDllPath))
	}
	return addr
}

func mightLoad(fun string) uintptr {
	addr, err := syscall.GetProcAddress(wpcapHandle, fun)
	if err != nil {
		return 0
	}
	return addr
}

func byteSliceToString(bval []byte) string {
	for i := range bval {
		if bval[i] == 0 {
			return string(bval[:i])
		}
	}
	return string(bval[:])
}

// bytePtrToString returns a string copied from pointer to a null terminated byte array
// WARNING: ONLY SAFE WITH IF r POINTS TO C MEMORY!
// govet will complain about this function for the reason stated above
func bytePtrToString(r uintptr) string {
	if r == 0 {
		return ""
	}
	bval := (*[1 << 30]byte)(unsafe.Pointer(r))
	return byteSliceToString(bval[:])
}

var wpcapHandle syscall.Handle
var msvcrtHandle syscall.Handle
var (
	callocPtr,
	pcapStrerrorPtr,
	pcapStatustostrPtr,
	pcapOpenLivePtr,
	pcapOpenOfflinePtr,
	pcapClosePtr,
	pcapGeterrPtr,
	pcapStatsPtr,
	pcapCompilePtr,
	pcapFreecodePtr,
	pcapLookupnetPtr,
	pcapOfflineFilterPtr,
	pcapSetfilterPtr,
	pcapListDatalinksPtr,
	pcapFreeDatalinksPtr,
	pcapDatalinkValToNamePtr,
	pcapDatalinkValToDescriptionPtr,
	pcapOpenDeadPtr,
	pcapNextExPtr,
	pcapDatalinkPtr,
	pcapSetDatalinkPtr,
	pcapDatalinkNameToValPtr,
	pcapLibVersionPtr,
	pcapFreealldevsPtr,
	pcapFindalldevsPtr,
	pcapSendpacketPtr,
	pcapSetdirectionPtr,
	pcapSnapshotPtr,
	pcapTstampTypeValToNamePtr,
	pcapTstampTypeNameToValPtr,
	pcapListTstampTypesPtr,
	pcapFreeTstampTypesPtr,
	pcapSetTstampTypePtr,
	pcapGetTstampPrecisionPtr,
	pcapSetTstampPrecisionPtr,
	pcapOpenOfflineWithTstampPrecisionPtr,
	pcapHOpenOfflineWithTstampPrecisionPtr,
	pcapActivatePtr,
	pcapCreatePtr,
	pcapSetSnaplenPtr,
	pcapSetPromiscPtr,
	pcapSetTimeoutPtr,
	pcapCanSetRfmonPtr,
	pcapSetRfmonPtr,
	pcapSetBufferSizePtr,
	pcapSetImmediateModePtr,
	pcapHopenOfflinePtr uintptr
)

func init() {
	kernel32, err := syscall.LoadLibrary("kernel32.dll")
	if err != nil {
		panic("couldn't load kernel32.dll")
	}
	defer syscall.FreeLibrary(kernel32)

	initDllPath(kernel32)

	wpcapHandle, err = syscall.LoadLibrary("wpcap.dll")
	if err != nil {
		panic("Couldn't load wpcap.dll")
	}
	initLoadedDllPath(kernel32)
	msvcrtHandle, err = syscall.LoadLibrary("msvcrt.dll")
	if err != nil {
		panic("Couldn't load msvcrt.dll")
	}
	callocPtr, err = syscall.GetProcAddress(msvcrtHandle, "calloc")
	if err != nil {
		panic("Couldn't get calloc function")
	}

	pcapStrerrorPtr = mustLoad("pcap_strerror")
	pcapStatustostrPtr = mightLoad("pcap_statustostr") // not available on winpcap
	pcapOpenLivePtr = mustLoad("pcap_open_live")
	pcapOpenOfflinePtr = mustLoad("pcap_open_offline")
	pcapClosePtr = mustLoad("pcap_close")
	pcapGeterrPtr = mustLoad("pcap_geterr")
	pcapStatsPtr = mustLoad("pcap_stats")
	pcapCompilePtr = mustLoad("pcap_compile")
	pcapFreecodePtr = mustLoad("pcap_freecode")
	pcapLookupnetPtr = mustLoad("pcap_lookupnet")
	pcapOfflineFilterPtr = mustLoad("pcap_offline_filter")
	pcapSetfilterPtr = mustLoad("pcap_setfilter")
	pcapListDatalinksPtr = mustLoad("pcap_list_datalinks")
	pcapFreeDatalinksPtr = mustLoad("pcap_free_datalinks")
	pcapDatalinkValToNamePtr = mustLoad("pcap_datalink_val_to_name")
	pcapDatalinkValToDescriptionPtr = mustLoad("pcap_datalink_val_to_description")
	pcapOpenDeadPtr = mustLoad("pcap_open_dead")
	pcapNextExPtr = mustLoad("pcap_next_ex")
	pcapDatalinkPtr = mustLoad("pcap_datalink")
	pcapSetDatalinkPtr = mustLoad("pcap_set_datalink")
	pcapDatalinkNameToValPtr = mustLoad("pcap_datalink_name_to_val")
	pcapLibVersionPtr = mustLoad("pcap_lib_version")
	pcapFreealldevsPtr = mustLoad("pcap_freealldevs")
	pcapFindalldevsPtr = mustLoad("pcap_findalldevs")
	pcapSendpacketPtr = mustLoad("pcap_sendpacket")
	pcapSetdirectionPtr = mustLoad("pcap_setdirection")
	pcapSnapshotPtr = mustLoad("pcap_snapshot")
	//libpcap <1.2 doesn't have pcap_*_tstamp_* functions
	pcapTstampTypeValToNamePtr = mightLoad("pcap_tstamp_type_val_to_name")
	pcapTstampTypeNameToValPtr = mightLoad("pcap_tstamp_type_name_to_val")
	pcapListTstampTypesPtr = mightLoad("pcap_list_tstamp_types")
	pcapFreeTstampTypesPtr = mightLoad("pcap_free_tstamp_types")
	pcapSetTstampTypePtr = mightLoad("pcap_set_tstamp_type")
	pcapGetTstampPrecisionPtr = mightLoad("pcap_get_tstamp_precision")
	pcapSetTstampPrecisionPtr = mightLoad("pcap_set_tstamp_precision")
	pcapOpenOfflineWithTstampPrecisionPtr = mightLoad("pcap_open_offline_with_tstamp_precision")
	pcapHOpenOfflineWithTstampPrecisionPtr = mightLoad("pcap_hopen_offline_with_tstamp_precision")
	pcapActivatePtr = mustLoad("pcap_activate")
	pcapCreatePtr = mustLoad("pcap_create")
	pcapSetSnaplenPtr = mustLoad("pcap_set_snaplen")
	pcapSetPromiscPtr = mustLoad("pcap_set_promisc")
	pcapSetTimeoutPtr = mustLoad("pcap_set_timeout")
	//winpcap does not support rfmon
	pcapCanSetRfmonPtr = mightLoad("pcap_can_set_rfmon")
	pcapSetRfmonPtr = mightLoad("pcap_set_rfmon")
	pcapSetBufferSizePtr = mustLoad("pcap_set_buffer_size")
	//libpcap <1.5 does not have pcap_set_immediate_mode
	pcapSetImmediateModePtr = mightLoad("pcap_set_immediate_mode")
	pcapHopenOfflinePtr = mustLoad("pcap_hopen_offline")
}

func (h *pcapPkthdr) getSec() int64 {
	return int64(h.Ts.Sec)
}

func (h *pcapPkthdr) getUsec() int64 {
	return int64(h.Ts.Usec)
}

func (h *pcapPkthdr) getLen() int {
	return int(h.Len)
}

func (h *pcapPkthdr) getCaplen() int {
	return int(h.Caplen)
}

func statusError(status pcapCint) error {
	var ret uintptr
	if pcapStatustostrPtr == 0 {
		ret, _, _ = syscall.Syscall(pcapStrerrorPtr, 1, uintptr(status), 0, 0)
	} else {
		ret, _, _ = syscall.Syscall(pcapStatustostrPtr, 1, uintptr(status), 0, 0)
	}
	return errors.New(bytePtrToString(ret))
}

func pcapGetTstampPrecision(cptr pcapTPtr) int {
	if pcapGetTstampPrecisionPtr == 0 {
		return pcapTstampPrecisionMicro
	}
	ret, _, _ := syscall.Syscall(pcapGetTstampPrecisionPtr, 1, uintptr(cptr), 0, 0)
	return int(pcapCint(ret))
}

func pcapSetTstampPrecision(cptr pcapTPtr, precision int) error {
	if pcapSetTstampPrecisionPtr == 0 {
		return errors.New("Not supported")
	}
	ret, _, _ := syscall.Syscall(pcapSetTstampPrecisionPtr, 2, uintptr(cptr), uintptr(precision), 0)
	if pcapCint(ret) < 0 {
		return errors.New("Not supported")
	}
	return nil
}

func pcapOpenLive(device string, snaplen int, pro int, timeout int) (*Handle, error) {
	buf := make([]byte, errorBufferSize)
	dev, err := syscall.BytePtrFromString(device)
	if err != nil {
		return nil, err
	}

	cptr, _, _ := syscall.Syscall6(pcapOpenLivePtr, 5, uintptr(unsafe.Pointer(dev)), uintptr(snaplen), uintptr(pro), uintptr(timeout), uintptr(unsafe.Pointer(&buf[0])), 0)

	if cptr == 0 {
		return nil, errors.New(byteSliceToString(buf))
	}
	return &Handle{cptr: pcapTPtr(cptr)}, nil
}

func openOffline(file string) (handle *Handle, err error) {
	buf := make([]byte, errorBufferSize)
	f, err := syscall.BytePtrFromString(file)
	if err != nil {
		return nil, err
	}

	var cptr uintptr
	if pcapOpenOfflineWithTstampPrecisionPtr == 0 {
		cptr, _, _ = syscall.Syscall(pcapOpenOfflinePtr, 2, uintptr(unsafe.Pointer(f)), uintptr(unsafe.Pointer(&buf[0])), 0)
	} else {
		cptr, _, _ = syscall.Syscall(pcapOpenOfflineWithTstampPrecisionPtr, 3, uintptr(unsafe.Pointer(f)), uintptr(pcapTstampPrecisionNano), uintptr(unsafe.Pointer(&buf[0])))
	}

	if cptr == 0 {
		return nil, errors.New(byteSliceToString(buf))
	}

	h := &Handle{cptr: pcapTPtr(cptr)}
	return h, nil
}

func (p *Handle) pcapClose() {
	if p.cptr != 0 {
		_, _, _ = syscall.Syscall(pcapClosePtr, 1, uintptr(p.cptr), 0, 0)
	}
	p.cptr = 0
}

func (p *Handle) pcapGeterr() error {
	ret, _, _ := syscall.Syscall(pcapGeterrPtr, 1, uintptr(p.cptr), 0, 0)
	return errors.New(bytePtrToString(ret))
}

func (p *Handle) pcapStats() (*Stats, error) {
	var cstats pcapStats
	ret, _, _ := syscall.Syscall(pcapStatsPtr, 2, uintptr(p.cptr), uintptr(unsafe.Pointer(&cstats)), 0)
	if pcapCint(ret) < 0 {
		return nil, p.pcapGeterr()
	}
	return &Stats{
		PacketsReceived:  int(cstats.Recv),
		PacketsDropped:   int(cstats.Drop),
		PacketsIfDropped: int(cstats.Ifdrop),
	}, nil
}

// for libpcap < 1.8 pcap_compile is NOT thread-safe, so protect it.
var pcapCompileMu sync.Mutex

func (p *Handle) pcapCompile(expr string, maskp uint32) (pcapBpfProgram, error) {
	var bpf pcapBpfProgram
	cexpr, err := syscall.BytePtrFromString(expr)
	if err != nil {
		return pcapBpfProgram{}, err
	}
	pcapCompileMu.Lock()
	defer pcapCompileMu.Unlock()
	res, _, _ := syscall.Syscall6(pcapCompilePtr, 5, uintptr(p.cptr), uintptr(unsafe.Pointer(&bpf)), uintptr(unsafe.Pointer(cexpr)), uintptr(1), uintptr(maskp), 0)
	if pcapCint(res) < 0 {
		return bpf, p.pcapGeterr()
	}
	return bpf, nil
}

func (p pcapBpfProgram) free() {
	_, _, _ = syscall.Syscall(pcapFreecodePtr, 1, uintptr(unsafe.Pointer(&p)), 0, 0)
}

func (p pcapBpfProgram) toBPFInstruction() []BPFInstruction {
	bpfInsn := (*[bpfInstructionBufferSize]pcapBpfInstruction)(unsafe.Pointer(p.Insns))[0:p.Len:p.Len]
	bpfInstruction := make([]BPFInstruction, len(bpfInsn), len(bpfInsn))

	for i, v := range bpfInsn {
		bpfInstruction[i].Code = v.Code
		bpfInstruction[i].Jt = v.Jt
		bpfInstruction[i].Jf = v.Jf
		bpfInstruction[i].K = v.K
	}
	return bpfInstruction
}

func pcapBpfProgramFromInstructions(bpfInstructions []BPFInstruction) pcapBpfProgram {
	var bpf pcapBpfProgram
	bpf.Len = uint32(len(bpfInstructions))
	cbpfInsns, _, _ := syscall.Syscall(callocPtr, 2, uintptr(len(bpfInstructions)), uintptr(unsafe.Sizeof(bpfInstructions[0])), 0)
	gbpfInsns := (*[bpfInstructionBufferSize]pcapBpfInstruction)(unsafe.Pointer(cbpfInsns))

	for i, v := range bpfInstructions {
		gbpfInsns[i].Code = v.Code
		gbpfInsns[i].Jt = v.Jt
		gbpfInsns[i].Jf = v.Jf
		gbpfInsns[i].K = v.K
	}

	bpf.Insns = (*pcapBpfInstruction)(unsafe.Pointer(cbpfInsns))
	return bpf
}

func pcapLookupnet(device string) (netp, maskp uint32, err error) {
	buf := make([]byte, errorBufferSize)
	dev, err := syscall.BytePtrFromString(device)
	if err != nil {
		return 0, 0, err
	}
	e, _, _ := syscall.Syscall6(pcapLookupnetPtr, 4, uintptr(unsafe.Pointer(dev)), uintptr(unsafe.Pointer(&netp)), uintptr(unsafe.Pointer(&maskp)), uintptr(unsafe.Pointer(&buf[0])), 0, 0)
	if pcapCint(e) < 0 {
		return 0, 0, errors.New(byteSliceToString(buf))
	}
	return
}

func (b *BPF) pcapOfflineFilter(ci gopacket.CaptureInfo, data []byte) bool {
	var hdr pcapPkthdr
	hdr.Ts.Sec = int32(ci.Timestamp.Unix())
	hdr.Ts.Usec = int32(ci.Timestamp.Nanosecond() / 1000)
	hdr.Caplen = uint32(len(data)) // Trust actual length over ci.Length.
	hdr.Len = uint32(ci.Length)
	e, _, _ := syscall.Syscall(pcapOfflineFilterPtr, 3, uintptr(unsafe.Pointer(&b.bpf)), uintptr(unsafe.Pointer(&hdr)), uintptr(unsafe.Pointer(&data[0])))
	return e != 0
}

func (p *Handle) pcapSetfilter(bpf pcapBpfProgram) error {
	e, _, _ := syscall.Syscall(pcapSetfilterPtr, 2, uintptr(p.cptr), uintptr(unsafe.Pointer(&bpf)), 0)
	if pcapCint(e) < 0 {
		return p.pcapGeterr()
	}
	return nil
}

func (p *Handle) pcapListDatalinks() (datalinks []Datalink, err error) {
	var dltbuf *pcapCint
	ret, _, _ := syscall.Syscall(pcapListDatalinksPtr, 2, uintptr(p.cptr), uintptr(unsafe.Pointer(&dltbuf)), 0)

	n := int(pcapCint(ret))

	if n < 0 {
		return nil, p.pcapGeterr()
	}
	defer syscall.Syscall(pcapFreeDatalinksPtr, 1, uintptr(unsafe.Pointer(dltbuf)), 0, 0)

	datalinks = make([]Datalink, n)

	dltArray := (*[1 << 28]pcapCint)(unsafe.Pointer(dltbuf))

	for i := 0; i < n; i++ {
		datalinks[i].Name = pcapDatalinkValToName(int((*dltArray)[i]))
		datalinks[i].Description = pcapDatalinkValToDescription(int((*dltArray)[i]))
	}

	return datalinks, nil
}

func pcapOpenDead(linkType layers.LinkType, captureLength int) (*Handle, error) {
	cptr, _, _ := syscall.Syscall(pcapOpenDeadPtr, 2, uintptr(linkType), uintptr(captureLength), 0)
	if cptr == 0 {
		return nil, errors.New("error opening dead capture")
	}

	return &Handle{cptr: pcapTPtr(cptr)}, nil
}

func (p *Handle) pcapNextPacketEx() NextError {
	r, _, _ := syscall.Syscall(pcapNextExPtr, 3, uintptr(p.cptr), uintptr(unsafe.Pointer(&p.pkthdr)), uintptr(unsafe.Pointer(&p.bufptr)))
	ret := pcapCint(r)
	// According to https://github.com/the-tcpdump-group/libpcap/blob/1131a7c26c6f4d4772e4a2beeaf7212f4dea74ac/pcap.c#L398-L406 ,
	// the return value of pcap_next_ex could be greater than 1 for success.
	// Let's just make it 1 if it comes bigger than 1.
	if ret > 1 {
		ret = 1
	}
	return NextError(ret)
}

func (p *Handle) pcapDatalink() layers.LinkType {
	ret, _, _ := syscall.Syscall(pcapDatalinkPtr, 1, uintptr(p.cptr), 0, 0)
	return layers.LinkType(ret)
}

func (p *Handle) pcapSetDatalink(dlt layers.LinkType) error {
	ret, _, _ := syscall.Syscall(pcapSetDatalinkPtr, 2, uintptr(p.cptr), uintptr(dlt), 0)
	if pcapCint(ret) < 0 {
		return p.pcapGeterr()
	}
	return nil
}

func pcapDatalinkValToName(dlt int) string {
	ret, _, _ := syscall.Syscall(pcapDatalinkValToNamePtr, 1, uintptr(dlt), 0, 0)
	return bytePtrToString(ret)
}

func pcapDatalinkValToDescription(dlt int) string {
	ret, _, _ := syscall.Syscall(pcapDatalinkValToDescriptionPtr, 1, uintptr(dlt), 0, 0)
	return bytePtrToString(ret)
}

func pcapDatalinkNameToVal(name string) int {
	cptr, err := syscall.BytePtrFromString(name)
	if err != nil {
		return 0
	}
	ret, _, _ := syscall.Syscall(pcapDatalinkNameToValPtr, 1, uintptr(unsafe.Pointer(cptr)), 0, 0)
	return int(pcapCint(ret))
}

func pcapLibVersion() string {
	ret, _, _ := syscall.Syscall(pcapLibVersionPtr, 0, 0, 0, 0)
	return bytePtrToString(ret)
}

func (p *Handle) isOpen() bool {
	return p.cptr != 0
}

type pcapDevices struct {
	all, cur *pcapIf
}

func (p pcapDevices) free() {
	syscall.Syscall(pcapFreealldevsPtr, 1, uintptr(unsafe.Pointer(p.all)), 0, 0)
}

func (p *pcapDevices) next() bool {
	if p.cur == nil {
		p.cur = p.all
		if p.cur == nil {
			return false
		}
		return true
	}
	if p.cur.Next == nil {
		return false
	}
	p.cur = p.cur.Next
	return true
}

func (p pcapDevices) name() string {
	return bytePtrToString(uintptr(unsafe.Pointer(p.cur.Name)))
}

func (p pcapDevices) description() string {
	return bytePtrToString(uintptr(unsafe.Pointer(p.cur.Description)))
}

func (p pcapDevices) flags() uint32 {
	return p.cur.Flags
}

type pcapAddresses struct {
	all, cur *pcapAddr
}

func (p *pcapAddresses) next() bool {
	if p.cur == nil {
		p.cur = p.all
		if p.cur == nil {
			return false
		}
		return true
	}
	if p.cur.Next == nil {
		return false
	}
	p.cur = p.cur.Next
	return true
}

func (p pcapAddresses) addr() *syscall.RawSockaddr {
	return p.cur.Addr
}

func (p pcapAddresses) netmask() *syscall.RawSockaddr {
	return p.cur.Netmask
}

func (p pcapAddresses) broadaddr() *syscall.RawSockaddr {
	return p.cur.Broadaddr
}

func (p pcapAddresses) dstaddr() *syscall.RawSockaddr {
	return p.cur.Dstaddr
}

func (p pcapDevices) addresses() pcapAddresses {
	return pcapAddresses{all: p.cur.Addresses}
}

func pcapFindAllDevs() (pcapDevices, error) {
	buf := make([]byte, errorBufferSize)
	var alldevsp pcapDevices

	ret, _, _ := syscall.Syscall(pcapFindalldevsPtr, 2, uintptr(unsafe.Pointer(&alldevsp.all)), uintptr(unsafe.Pointer(&buf[0])), 0)

	if pcapCint(ret) < 0 {
		return pcapDevices{}, errors.New(byteSliceToString(buf))
	}
	return alldevsp, nil
}

func (p *Handle) pcapSendpacket(data []byte) error {
	ret, _, _ := syscall.Syscall(pcapSendpacketPtr, 3, uintptr(p.cptr), uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)))
	if pcapCint(ret) < 0 {
		return p.pcapGeterr()
	}
	return nil
}

func (p *Handle) pcapSetdirection(direction Direction) error {
	status, _, _ := syscall.Syscall(pcapSetdirectionPtr, 2, uintptr(p.cptr), uintptr(direction), 0)
	if pcapCint(status) < 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *Handle) pcapSnapshot() int {
	ret, _, _ := syscall.Syscall(pcapSnapshotPtr, 1, uintptr(p.cptr), 0, 0)
	return int(pcapCint(ret))
}

func (t TimestampSource) pcapTstampTypeValToName() string {
	//libpcap <1.2 doesn't have pcap_*_tstamp_* functions
	if pcapTstampTypeValToNamePtr == 0 {
		return "pcap timestamp types not supported"
	}
	ret, _, _ := syscall.Syscall(pcapTstampTypeValToNamePtr, 1, uintptr(t), 0, 0)
	return bytePtrToString(ret)
}

func pcapTstampTypeNameToVal(s string) (TimestampSource, error) {
	//libpcap <1.2 doesn't have pcap_*_tstamp_* functions
	if pcapTstampTypeNameToValPtr == 0 {
		return 0, statusError(pcapCint(pcapError))
	}
	cs, err := syscall.BytePtrFromString(s)
	if err != nil {
		return 0, err
	}
	ret, _, _ := syscall.Syscall(pcapTstampTypeNameToValPtr, 1, uintptr(unsafe.Pointer(cs)), 0, 0)
	t := pcapCint(ret)
	if t < 0 {
		return 0, statusError(pcapCint(t))
	}
	return TimestampSource(t), nil
}

func (p *InactiveHandle) pcapGeterr() error {
	ret, _, _ := syscall.Syscall(pcapGeterrPtr, 1, uintptr(p.cptr), 0, 0)
	return errors.New(bytePtrToString(ret))
}

func (p *InactiveHandle) pcapActivate() (*Handle, activateError) {
	r, _, _ := syscall.Syscall(pcapActivatePtr, 1, uintptr(p.cptr), 0, 0)
	ret := activateError(pcapCint(r))
	if ret != aeNoError {
		return nil, ret
	}
	h := &Handle{
		cptr: p.cptr,
	}
	p.cptr = 0
	return h, ret
}

func (p *InactiveHandle) pcapClose() {
	if p.cptr != 0 {
		_, _, _ = syscall.Syscall(pcapClosePtr, 1, uintptr(p.cptr), 0, 0)
	}
	p.cptr = 0
}

func pcapCreate(device string) (*InactiveHandle, error) {
	buf := make([]byte, errorBufferSize)
	dev, err := syscall.BytePtrFromString(device)
	if err != nil {
		return nil, err
	}
	cptr, _, _ := syscall.Syscall(pcapCreatePtr, 2, uintptr(unsafe.Pointer(dev)), uintptr(unsafe.Pointer(&buf[0])), 0)
	if cptr == 0 {
		return nil, errors.New(byteSliceToString(buf))
	}
	return &InactiveHandle{cptr: pcapTPtr(cptr)}, nil
}

func (p *InactiveHandle) pcapSetSnaplen(snaplen int) error {
	status, _, _ := syscall.Syscall(pcapSetSnaplenPtr, 2, uintptr(p.cptr), uintptr(snaplen), 0)
	if pcapCint(status) < 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *InactiveHandle) pcapSetPromisc(promisc bool) error {
	var pro uintptr
	if promisc {
		pro = 1
	}
	status, _, _ := syscall.Syscall(pcapSetPromiscPtr, 2, uintptr(p.cptr), pro, 0)
	if pcapCint(status) < 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *InactiveHandle) pcapSetTimeout(timeout time.Duration) error {
	status, _, _ := syscall.Syscall(pcapSetTimeoutPtr, 2, uintptr(p.cptr), uintptr(timeoutMillis(timeout)), 0)

	if pcapCint(status) < 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *InactiveHandle) pcapListTstampTypes() (out []TimestampSource) {
	//libpcap <1.2 doesn't have pcap_*_tstamp_* functions
	if pcapListTstampTypesPtr == 0 {
		return
	}
	var types *pcapCint
	ret, _, _ := syscall.Syscall(pcapListTstampTypesPtr, 2, uintptr(p.cptr), uintptr(unsafe.Pointer(&types)), 0)
	n := int(pcapCint(ret))
	if n < 0 {
		return // public interface doesn't have error :(
	}
	defer syscall.Syscall(pcapFreeTstampTypesPtr, 1, uintptr(unsafe.Pointer(types)), 0, 0)
	typesArray := (*[1 << 28]pcapCint)(unsafe.Pointer(types))
	for i := 0; i < n; i++ {
		out = append(out, TimestampSource((*typesArray)[i]))
	}
	return
}

func (p *InactiveHandle) pcapSetTstampType(t TimestampSource) error {
	//libpcap <1.2 doesn't have pcap_*_tstamp_* functions
	if pcapSetTstampTypePtr == 0 {
		return statusError(pcapError)
	}
	status, _, _ := syscall.Syscall(pcapSetTstampTypePtr, 2, uintptr(p.cptr), uintptr(t), 0)
	if pcapCint(status) < 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *InactiveHandle) pcapSetRfmon(monitor bool) error {
	//winpcap does not support rfmon
	if pcapCanSetRfmonPtr == 0 {
		return CannotSetRFMon
	}
	var mon uintptr
	if monitor {
		mon = 1
	}
	canset, _, _ := syscall.Syscall(pcapCanSetRfmonPtr, 1, uintptr(p.cptr), 0, 0)
	switch canset {
	case 0:
		return CannotSetRFMon
	case 1:
		// success
	default:
		return statusError(pcapCint(canset))
	}
	status, _, _ := syscall.Syscall(pcapSetRfmonPtr, 2, uintptr(p.cptr), mon, 0)
	if status != 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *InactiveHandle) pcapSetBufferSize(bufferSize int) error {
	status, _, _ := syscall.Syscall(pcapSetBufferSizePtr, 2, uintptr(p.cptr), uintptr(bufferSize), 0)
	if pcapCint(status) < 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *InactiveHandle) pcapSetImmediateMode(mode bool) error {
	//libpcap <1.5 does not have pcap_set_immediate_mode
	if pcapSetImmediateModePtr == 0 {
		return statusError(pcapError)
	}
	var md uintptr
	if mode {
		md = 1
	}
	status, _, _ := syscall.Syscall(pcapSetImmediateModePtr, 2, uintptr(p.cptr), md, 0)
	if pcapCint(status) < 0 {
		return statusError(pcapCint(status))
	}
	return nil
}

func (p *Handle) setNonBlocking() error {
	// do nothing
	return nil
}

// waitForPacket waits for a packet or for the timeout to expire.
func (p *Handle) waitForPacket() {
	// can't use select() so instead just switch goroutines
	runtime.Gosched()
}

// openOfflineFile returns contents of input file as a *Handle.
func openOfflineFile(file *os.File) (handle *Handle, err error) {
	buf := make([]byte, errorBufferSize)
	cf := file.Fd()

	var cptr uintptr
	if pcapOpenOfflineWithTstampPrecisionPtr == 0 {
		cptr, _, _ = syscall.Syscall(pcapHopenOfflinePtr, 2, cf, uintptr(unsafe.Pointer(&buf[0])), 0)
	} else {
		cptr, _, _ = syscall.Syscall(pcapHOpenOfflineWithTstampPrecisionPtr, 3, cf, uintptr(pcapTstampPrecisionNano), uintptr(unsafe.Pointer(&buf[0])))
	}

	if cptr == 0 {
		return nil, errors.New(byteSliceToString(buf))
	}
	return &Handle{cptr: pcapTPtr(cptr)}, nil
}
