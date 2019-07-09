// +build js

package syscall

import (
	"github.com/gopherjs/gopherjs/js"
)

func funcPC(f func()) uintptr {
	if js.InternalObject(f) == js.InternalObject(libc_write_trampoline) {
		return SYS_WRITE
	}
	return uintptr(minusOne)
}

func syscall(trap, a1, a2, a3 uintptr) (r1, r2 uintptr, err Errno) {
	return Syscall(trap, a1, a2, a3)
}

func syscall6(trap, a1, a2, a3, a4, a5, a6 uintptr) (r1, r2 uintptr, err Errno) {
	return Syscall6(trap, a1, a2, a3, a4, a5, a6)
}

func syscall6X(trap, a1, a2, a3, a4, a5, a6 uintptr) (r1, r2 uintptr, err Errno) {
	panic("syscall6X is not implemented")
}

func rawSyscall(trap, a1, a2, a3 uintptr) (r1, r2 uintptr, err Errno) {
	return RawSyscall(trap, a1, a2, a3)
}

func rawSyscall6(trap, a1, a2, a3, a4, a5, a6 uintptr) (r1, r2 uintptr, err Errno) {
	return RawSyscall6(trap, a1, a2, a3, a4, a5, a6)
}
