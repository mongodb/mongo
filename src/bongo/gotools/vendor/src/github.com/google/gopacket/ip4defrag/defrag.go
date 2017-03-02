// Copyright 2013 Google, Inc. All rights reserved.
//
// Package ip4defrag implements a IPv4 defragmenter
package ip4defrag

import (
	"container/list"
	"fmt"
	"log"
	"sync"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

// Quick and Easy to use debug code to trace
// how defrag works.
var debug debugging = false // or flip to false
type debugging bool

func (d debugging) Printf(format string, args ...interface{}) {
	if d {
		log.Printf(format, args...)
	}
}

const (
	IPv4MinimumFragmentSize    = 576
	IPv4MaximumSize            = 65535
	IPv4MaximumFragmentOffset  = 8189
	IPv4MaximumFragmentListLen = 8
)

// DefragIPv4 takes in an IPv4 packet with a fragment payload.
//
// It do not modify the IPv4 layer in place, 'in' remains untouched
// It returns a ready-to be used IPv4 layer.
//
// If the passed-in IPv4 layer is NOT fragmented, it will
// immediately return it without modifying the layer.
//
// If the IPv4 layer is a fragment and we don't have all
// fragments, it will return nil and store whatever internal
// information it needs to eventually defrag the packet.
//
// If the IPv4 layer is the last fragment needed to reconstruct
// the packet, a new IPv4 layer will be returned, and will be set to
// the entire defragmented packet,
//
// It use a map of all the running flows
//
// Usage example:
//
// func HandlePacket(in *layers.IPv4) err {
//     defragger := ip4defrag.NewIPv4Defragmenter()
//     in, err := defragger.DefragIPv4(in)
//     if err != nil {
//         return err
//     } else if in == nil {
//         return nil  // packet fragment, we don't have whole packet yet.
//     }
//     // At this point, we know that 'in' is defragmented.
//     //It may be the same 'in' passed to
//	   // HandlePacket, or it may not, but we don't really care :)
//	   ... do stuff to 'in' ...
//}
//
func (d *IPv4Defragmenter) DefragIPv4(in *layers.IPv4) (*layers.IPv4, error) {
	// check if we need to defrag
	if st := d.dontDefrag(in); st == true {
		return in, nil
	}
	// perfom security checks
	st, err := d.securityChecks(in)
	if err != nil || st == false {
		return nil, err
	}

	// ok, got a fragment
	debug.Printf("defrag: got in.Id=%d in.FragOffset=%d in.Flags=%d\n",
		in.Id, in.FragOffset*8, in.Flags)

	// do we already has seen a flow between src/dst with that Id
	ipf := newIPv4(in)
	var fl *fragmentList
	var exist bool
	d.Lock()
	fl, exist = d.ipFlows[ipf]
	if !exist {
		debug.Printf("defrag: creating a new flow\n")
		fl = new(fragmentList)
		d.ipFlows[ipf] = fl
	}
	d.Unlock()
	// insert, and if final build it
	out, err2 := fl.insert(in)

	// at last, if we hit the maximum frag list len
	// without any defrag success, we just drop everything and
	// raise an error
	if out == nil && fl.List.Len()+1 > IPv4MaximumFragmentListLen {
		d.Lock()
		fl = new(fragmentList)
		d.ipFlows[ipf] = fl
		d.Unlock()
		return nil, fmt.Errorf("defrag: Fragment List hits its maximum"+
			"size(%d), without sucess. Flushing the list",
			IPv4MaximumFragmentListLen)
	}

	// if we got a packet, it's a new one, and he is defragmented
	if out != nil {
		return out, nil
	}
	return nil, err2
}

// DiscardOlderThan forgets all packets without any activity since
// time t. It returns the number of FragmentList aka number of
// fragment packets it has discarded.
func (d *IPv4Defragmenter) DiscardOlderThan(t time.Time) int {
	var nb int
	d.Lock()
	for k, v := range d.ipFlows {
		if v.LastSeen.Before(t) {
			nb = nb + 1
			delete(d.ipFlows, k)
		}
	}
	d.Unlock()
	return nb
}

// dontDefrag returns true if the IPv4 packet do not need
// any defragmentation
func (d *IPv4Defragmenter) dontDefrag(ip *layers.IPv4) bool {
	// don't defrag packet with DF flag
	if ip.Flags&layers.IPv4DontFragment != 0 {
		return true
	}
	// don't defrag not fragmented ones
	if ip.Flags&layers.IPv4MoreFragments == 0 && ip.FragOffset == 0 {
		return true
	}
	return false
}

// securityChecks performs the needed security checks
func (d *IPv4Defragmenter) securityChecks(ip *layers.IPv4) (bool, error) {
	// don't allow too big fragment offset
	if ip.FragOffset > IPv4MaximumFragmentOffset {
		return false, fmt.Errorf("defrag: fragment offset too big "+
			"(handcrafted? %d > %d)", ip.FragOffset, IPv4MaximumFragmentOffset)
	}
	fragOffset := ip.FragOffset * 8

	// don't allow fragment that would oversize an IP packet
	if fragOffset+ip.Length > IPv4MaximumSize {
		return false, fmt.Errorf("defrag: fragment will overrun "+
			"(handcrafted? %d > %d)", ip.FragOffset*8+ip.Length, IPv4MaximumSize)
	}

	return true, nil
}

// fragmentList holds a container/list used to contains IP
// packets/fragments.  It stores internal counters to track the
// maximum total of byte, and the current length it has received.
// It also stores a flag to know if he has seen the last packet.
type fragmentList struct {
	List          list.List
	Highest       uint16
	Current       uint16
	FinalReceived bool
	LastSeen      time.Time
}

// insert insert an IPv4 fragment/packet into the Fragment List
// It use the following strategy : we are inserting fragment based
// on their offset, latest first. This is sometimes called BSD-Right.
// See: http://www.sans.org/reading-room/whitepapers/detection/ip-fragment-reassembly-scapy-33969
func (f *fragmentList) insert(in *layers.IPv4) (*layers.IPv4, error) {
	// TODO: should keep a copy of *in in the list
	// or not (ie the packet source is reliable) ?
	fragOffset := in.FragOffset * 8
	if fragOffset >= f.Highest {
		f.List.PushBack(in)
	} else {
		for e := f.List.Front(); e != nil; e = e.Next() {
			frag, _ := e.Value.(*layers.IPv4)
			if in.FragOffset <= frag.FragOffset {
				debug.Printf("defrag: inserting frag %d before existing frag %d \n",
					fragOffset, frag.FragOffset*8)
				f.List.InsertBefore(in, e)
				break
			}
		}
	}
	// packet.Metadata().Timestamp should have been better, but
	// we don't have this info there...
	f.LastSeen = time.Now()

	fragLength := in.Length - 20
	// After inserting the Fragment, we update the counters
	if f.Highest < fragOffset+fragLength {
		f.Highest = fragOffset + fragLength
	}
	f.Current = f.Current + fragLength

	debug.Printf("defrag: insert ListLen: %d Highest:%d Current:%d\n",
		f.List.Len(),
		f.Highest, f.Current)

	// Final Fragment ?
	if in.Flags&layers.IPv4MoreFragments == 0 {
		f.FinalReceived = true
	}
	// Ready to try defrag ?
	if f.FinalReceived && f.Highest == f.Current {
		return f.build(in)
	}
	return nil, nil
}

// Build builds the final datagram, modifying ip in place.
// It puts priority to packet in the early position of the list.
// See Insert for more details.
func (f *fragmentList) build(in *layers.IPv4) (*layers.IPv4, error) {
	var final []byte
	var currentOffset uint16 = 0

	debug.Printf("defrag: building the datagram \n")
	for e := f.List.Front(); e != nil; e = e.Next() {
		frag, _ := e.Value.(*layers.IPv4)
		if frag.FragOffset*8 == currentOffset {
			debug.Printf("defrag: building - adding %d\n", frag.FragOffset*8)
			final = append(final, frag.Payload...)
			currentOffset = currentOffset + frag.Length - 20
		} else if frag.FragOffset*8 < currentOffset {
			// overlapping fragment - let's take only what we need
			startAt := currentOffset - frag.FragOffset*8
			debug.Printf("defrag: building - overlapping, starting at %d\n",
				startAt)
			if startAt > frag.Length-20 {
				return nil, fmt.Errorf("defrag: building - invalid fragment")
			}
			final = append(final, frag.Payload[startAt:]...)
			currentOffset = currentOffset + frag.FragOffset*8
		} else {
			// Houston - we have an hole !
			debug.Printf("defrag: hole found while building, " +
				"stopping the defrag process\n")
			return nil, fmt.Errorf("defrag: building - hole found")
		}
		debug.Printf("defrag: building - next is %d\n", currentOffset)
	}

	// TODO recompute IP Checksum
	out := &layers.IPv4{
		Version:    in.Version,
		IHL:        in.IHL,
		TOS:        in.TOS,
		Length:     f.Highest,
		Id:         0,
		Flags:      0,
		FragOffset: 0,
		TTL:        in.TTL,
		Protocol:   in.Protocol,
		Checksum:   0,
		SrcIP:      in.SrcIP,
		DstIP:      in.DstIP,
		Options:    in.Options,
		Padding:    in.Padding,
	}
	out.Payload = final

	return out, nil
}

// ipv4 is a struct to be used as a key.
type ipv4 struct {
	ip4 gopacket.Flow
	id  uint16
}

// newIPv4 returns a new initialized IPv4 Flow
func newIPv4(ip *layers.IPv4) ipv4 {
	return ipv4{
		ip4: ip.NetworkFlow(),
		id:  ip.Id,
	}
}

// IPv4Defragmenter is a struct which embedded a map of
// all fragment/packet.
type IPv4Defragmenter struct {
	sync.RWMutex
	ipFlows map[ipv4]*fragmentList
}

// NewIPv4Defragmenter returns a new IPv4Defragmenter
// with an initialized map.
func NewIPv4Defragmenter() *IPv4Defragmenter {
	return &IPv4Defragmenter{
		ipFlows: make(map[ipv4]*fragmentList),
	}
}
