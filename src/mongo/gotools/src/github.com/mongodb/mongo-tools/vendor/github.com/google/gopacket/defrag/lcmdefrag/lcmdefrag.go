// Copyright 2018 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// Package lcmdefrag contains a defragmenter for LCM messages.
package lcmdefrag

import (
	"fmt"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

const (
	// Packages are cleaned up/removed after no input was received for this
	// amount of seconds.
	timeout time.Duration = 3 * time.Second
)

type lcmPacket struct {
	lastPacket time.Time
	done       bool
	recFrags   uint16
	totalFrags uint16
	frags      map[uint16]*layers.LCM
}

// LCMDefragmenter supports defragmentation of LCM messages.
//
// References
//   https://lcm-proj.github.io/
//   https://github.com/lcm-proj/lcm
type LCMDefragmenter struct {
	packets map[uint32]*lcmPacket
}

func newLCMPacket(totalFrags uint16) *lcmPacket {
	return &lcmPacket{
		done:       false,
		recFrags:   0,
		totalFrags: totalFrags,
		frags:      make(map[uint16]*layers.LCM),
	}
}

// NewLCMDefragmenter returns a new LCMDefragmenter.
func NewLCMDefragmenter() *LCMDefragmenter {
	return &LCMDefragmenter{
		packets: make(map[uint32]*lcmPacket),
	}
}

func (lp *lcmPacket) append(in *layers.LCM) {
	lp.frags[in.FragmentNumber] = in
	lp.recFrags++
	lp.lastPacket = time.Now()
}

func (lp *lcmPacket) assemble() (out *layers.LCM, err error) {
	var blob []byte

	//Extract packets
	for i := uint16(0); i < lp.totalFrags; i++ {
		fragment, ok := lp.frags[i]
		if !ok {
			err = fmt.Errorf("Tried to defragment incomplete packet. Waiting "+
				"for more potential (unordered) packets... %d", i)
			return
		}

		// For the very first packet, we also want the header.
		if i == 0 {
			blob = append(blob, fragment.LayerContents()...)
		}

		// Append the data for each packet.
		blob = append(blob, fragment.Payload()...)
	}

	packet := gopacket.NewPacket(blob, layers.LayerTypeLCM, gopacket.NoCopy)
	lcmHdrLayer := packet.Layer(layers.LayerTypeLCM)
	out, ok := lcmHdrLayer.(*layers.LCM)
	if !ok {
		err = fmt.Errorf("Error while decoding the defragmented packet. " +
			"Erasing/dropping packet.")
	}

	lp.done = true

	return
}

func (ld *LCMDefragmenter) cleanUp() {
	for key, packet := range ld.packets {
		if packet.done || time.Now().Sub(packet.lastPacket) > timeout {
			delete(ld.packets, key)
		}
	}
}

// Defrag takes a reference to an LCM packet and processes it.
// In case the packet does not need to be defragmented, it immediately returns
// the as in passed reference. In case in was the last missing fragment, out
// will be the defragmented packet. If in was a fragment, but we are awaiting
// more, out will be set to nil.
// In the case that in was nil, we will just run the internal cleanup of the
// defragmenter that times out packages.
// If an error was encountered during defragmentation, out will also be nil,
// while err will contain further information on the failure.
func (ld *LCMDefragmenter) Defrag(in *layers.LCM) (out *layers.LCM, err error) {
	// Timeout old packages and erase error prone ones.
	ld.cleanUp()

	// For running cleanup only
	if in == nil {
		return
	}

	// Quick check if this is acutally a single packet. In that case, just
	// return it quickly.
	if !in.Fragmented {
		out = in
		return
	}

	// Do we need to start a new fragments obj?
	if _, ok := ld.packets[in.SequenceNumber]; !ok {
		ld.packets[in.SequenceNumber] = newLCMPacket(in.TotalFragments)
	}

	// Append the packet
	ld.packets[in.SequenceNumber].append(in)

	// Check if this is the last package of that series
	if ld.packets[in.SequenceNumber].recFrags == in.TotalFragments {
		out, err = ld.packets[in.SequenceNumber].assemble()
	}

	return
}
