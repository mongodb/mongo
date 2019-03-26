// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package reassembly

import (
	"encoding/binary"
	"fmt"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

/*
 * Check TCP packet against options (window, MSS)
 */

type tcpStreamOptions struct {
	mss           int
	scale         int
	receiveWindow uint
}

// TCPOptionCheck contains options for the two directions
type TCPOptionCheck struct {
	options [2]tcpStreamOptions
}

func (t *TCPOptionCheck) getOptions(dir TCPFlowDirection) *tcpStreamOptions {
	if dir == TCPDirClientToServer {
		return &t.options[0]
	}
	return &t.options[1]
}

// NewTCPOptionCheck creates default options
func NewTCPOptionCheck() TCPOptionCheck {
	return TCPOptionCheck{
		options: [2]tcpStreamOptions{
			tcpStreamOptions{
				mss:           0,
				scale:         -1,
				receiveWindow: 0,
			}, tcpStreamOptions{
				mss:           0,
				scale:         -1,
				receiveWindow: 0,
			},
		},
	}
}

// Accept checks whether the packet should be accepted by checking TCP options
func (t *TCPOptionCheck) Accept(tcp *layers.TCP, ci gopacket.CaptureInfo, dir TCPFlowDirection, nextSeq Sequence, start *bool) error {
	options := t.getOptions(dir)
	if tcp.SYN {
		mss := -1
		scale := -1
		for _, o := range tcp.Options {
			// MSS
			if o.OptionType == 2 {
				if len(o.OptionData) != 2 {
					return fmt.Errorf("MSS option data length expected 2, got %d", len(o.OptionData))
				}
				mss = int(binary.BigEndian.Uint16(o.OptionData[:2]))
			}
			// Window scaling
			if o.OptionType == 3 {
				if len(o.OptionData) != 1 {
					return fmt.Errorf("Window scaling length expected: 1, got %d", len(o.OptionData))
				}
				scale = int(o.OptionData[0])
			}
		}
		options.mss = mss
		options.scale = scale
	} else {
		if nextSeq != invalidSequence {
			revOptions := t.getOptions(dir.Reverse())
			length := len(tcp.Payload)

			// Check packet is in the correct window
			diff := nextSeq.Difference(Sequence(tcp.Seq))
			if diff == -1 && (length == 1 || length == 0) {
				// This is probably a Keep-alive
				// TODO: check byte is ok
			} else if diff < 0 {
				return fmt.Errorf("Re-emitted packet (diff:%d,seq:%d,rev-ack:%d)", diff,
					tcp.Seq, nextSeq)
			} else if revOptions.mss > 0 && length > revOptions.mss {
				return fmt.Errorf("%d > mss (%d)", length, revOptions.mss)
			} else if revOptions.receiveWindow != 0 && revOptions.scale < 0 && diff > int(revOptions.receiveWindow) {
				return fmt.Errorf("%d > receiveWindow(%d)", diff, revOptions.receiveWindow)
			}
		}
	}
	// Compute receiveWindow
	options.receiveWindow = uint(tcp.Window)
	if options.scale > 0 {
		options.receiveWindow = options.receiveWindow << (uint(options.scale))
	}
	return nil
}

// TCPSimpleFSM implements a very simple TCP state machine
//
// Usage:
// When implementing a Stream interface and to avoid to consider packets that
// would be rejected due to client/server's TCP stack, the  Accept() can call
// TCPSimpleFSM.CheckState().
//
// Limitations:
// - packet should be received in-order.
// - no check on sequence number is performed
// - no RST
type TCPSimpleFSM struct {
	dir     TCPFlowDirection
	state   int
	options TCPSimpleFSMOptions
}

// TCPSimpleFSMOptions holds options for TCPSimpleFSM
type TCPSimpleFSMOptions struct {
	SupportMissingEstablishment bool // Allow missing SYN, SYN+ACK, ACK
}

// Internal values of state machine
const (
	TCPStateClosed      = 0
	TCPStateSynSent     = 1
	TCPStateEstablished = 2
	TCPStateCloseWait   = 3
	TCPStateLastAck     = 4
	TCPStateReset       = 5
)

// NewTCPSimpleFSM creates a new TCPSimpleFSM
func NewTCPSimpleFSM(options TCPSimpleFSMOptions) *TCPSimpleFSM {
	return &TCPSimpleFSM{
		state:   TCPStateClosed,
		options: options,
	}
}

func (t *TCPSimpleFSM) String() string {
	switch t.state {
	case TCPStateClosed:
		return "Closed"
	case TCPStateSynSent:
		return "SynSent"
	case TCPStateEstablished:
		return "Established"
	case TCPStateCloseWait:
		return "CloseWait"
	case TCPStateLastAck:
		return "LastAck"
	case TCPStateReset:
		return "Reset"
	}
	return "?"
}

// CheckState returns false if tcp is invalid wrt current state or update the state machine's state
func (t *TCPSimpleFSM) CheckState(tcp *layers.TCP, dir TCPFlowDirection) bool {
	if t.state == TCPStateClosed && t.options.SupportMissingEstablishment && !(tcp.SYN && !tcp.ACK) {
		/* try to figure out state */
		switch true {
		case tcp.SYN && tcp.ACK:
			t.state = TCPStateSynSent
			t.dir = dir.Reverse()
		case tcp.FIN && !tcp.ACK:
			t.state = TCPStateEstablished
		case tcp.FIN && tcp.ACK:
			t.state = TCPStateCloseWait
			t.dir = dir.Reverse()
		default:
			t.state = TCPStateEstablished
		}
	}

	switch t.state {
	/* openning connection */
	case TCPStateClosed:
		if tcp.SYN && !tcp.ACK {
			t.dir = dir
			t.state = TCPStateSynSent
			return true
		}
	case TCPStateSynSent:
		if tcp.RST {
			t.state = TCPStateReset
			return true
		}

		if tcp.SYN && tcp.ACK && dir == t.dir.Reverse() {
			t.state = TCPStateEstablished
			return true
		}
		if tcp.SYN && !tcp.ACK && dir == t.dir {
			// re-transmission
			return true
		}
	/* established */
	case TCPStateEstablished:
		if tcp.RST {
			t.state = TCPStateReset
			return true
		}

		if tcp.FIN {
			t.state = TCPStateCloseWait
			t.dir = dir
			return true
		}
		// accept any packet
		return true
	/* closing connection */
	case TCPStateCloseWait:
		if tcp.RST {
			t.state = TCPStateReset
			return true
		}

		if tcp.FIN && tcp.ACK && dir == t.dir.Reverse() {
			t.state = TCPStateLastAck
			return true
		}
		if tcp.ACK {
			return true
		}
	case TCPStateLastAck:
		if tcp.RST {
			t.state = TCPStateReset
			return true
		}

		if tcp.ACK && t.dir == dir {
			t.state = TCPStateClosed
			return true
		}
	}
	return false
}
