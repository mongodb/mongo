// +build windows

package gopass

import "syscall"
import "unsafe"
import "unicode/utf16"

func getch() byte {
	modkernel32 := syscall.NewLazyDLL("kernel32.dll")
	procReadConsole := modkernel32.NewProc("ReadConsoleW")
	procGetConsoleMode := modkernel32.NewProc("GetConsoleMode")
	procSetConsoleMode := modkernel32.NewProc("SetConsoleMode")

	var mode uint32
	pMode := &mode
	procGetConsoleMode.Call(uintptr(syscall.Stdin), uintptr(unsafe.Pointer(pMode)))

	var echoMode, lineMode uint32
	echoMode = 4
	lineMode = 2
	var newMode uint32
	newMode = mode ^ (echoMode | lineMode)

	procSetConsoleMode.Call(uintptr(syscall.Stdin), uintptr(newMode))

	line := make([]uint16, 1)
	pLine := &line[0]
	var n uint16
	procReadConsole.Call(uintptr(syscall.Stdin), uintptr(unsafe.Pointer(pLine)), uintptr(len(line)), uintptr(unsafe.Pointer(&n)))

	b := []byte(string(utf16.Decode(line)))

	procSetConsoleMode.Call(uintptr(syscall.Stdin), uintptr(mode))

	// Not sure how this could happen, but it did for someone
	if len(b) > 0 {
		return b[0]
	} else {
		return 13
	}
}
