// +build js

package poll

import "time"

// pollDesc is a no-op implementation of an I/O poller for GOARCH=js.
//
// Its implementation is based on NaCL in gc compiler (see GOROOT/src/internal/poll/fd_poll_nacl.go),
// but it does even less.
type pollDesc struct {
	closing bool
}

func (pd *pollDesc) init(fd *FD) error { return nil }

func (pd *pollDesc) close() {}

func (pd *pollDesc) evict() { pd.closing = true }

func (pd *pollDesc) prepare(mode int, isFile bool) error {
	if pd.closing {
		return errClosing(isFile)
	}
	return nil
}

func (pd *pollDesc) prepareRead(isFile bool) error { return pd.prepare('r', isFile) }

func (pd *pollDesc) prepareWrite(isFile bool) error { return pd.prepare('w', isFile) }

func (pd *pollDesc) wait(mode int, isFile bool) error {
	if pd.closing {
		return errClosing(isFile)
	}
	return ErrTimeout
}

func (pd *pollDesc) waitRead(isFile bool) error { return pd.wait('r', isFile) }

func (pd *pollDesc) waitWrite(isFile bool) error { return pd.wait('w', isFile) }

func (*pollDesc) waitCanceled(mode int) {}

func (*pollDesc) pollable() bool { return true }

func (*FD) SetDeadline(t time.Time) error { return nil }

func (*FD) SetReadDeadline(t time.Time) error { return nil }

func (*FD) SetWriteDeadline(t time.Time) error { return nil }

// PollDescriptor returns the descriptor being used by the poller,
// or ^uintptr(0) if there isn't one. This is only used for testing.
func PollDescriptor() uintptr {
	return ^uintptr(0)
}

// Copy of sync.runtime_Semacquire.
func runtime_Semacquire(s *uint32) {
	if *s == 0 {
		ch := make(chan bool)
		semWaiters[s] = append(semWaiters[s], ch)
		<-ch
	}
	*s--
}

// Copy of sync.runtime_Semrelease.
func runtime_Semrelease(s *uint32) {
	*s++

	w := semWaiters[s]
	if len(w) == 0 {
		return
	}

	ch := w[0]
	w = w[1:]
	semWaiters[s] = w
	if len(w) == 0 {
		delete(semWaiters, s)
	}

	ch <- true
}

var semWaiters = make(map[*uint32][]chan bool)
