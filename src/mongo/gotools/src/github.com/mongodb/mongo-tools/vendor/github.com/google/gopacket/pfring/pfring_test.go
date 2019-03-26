// Copyright 2019 The GoPacket Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pfring

import (
	"flag"
	"log"
	"testing"
)

var iface = flag.String("i", "eth0", "Interface to read packets from")

func BenchmarkPfringRead(b *testing.B) {
	var ring *Ring
	var err error
	if ring, err = NewRing(*iface, 65536, FlagPromisc); err != nil {
		log.Fatalln("pfring ring creation error:", err)
	}
	if err = ring.SetSocketMode(ReadOnly); err != nil {
		log.Fatalln("pfring SetSocketMode error:", err)
	} else if err = ring.Enable(); err != nil {
		log.Fatalln("pfring Enable error:", err)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_, ci, _ := ring.ReadPacketData()
		b.SetBytes(int64(ci.CaptureLength))
	}
}

func BenchmarkPfringReadZero(b *testing.B) {
	var ring *Ring
	var err error
	if ring, err = NewRing(*iface, 65536, FlagPromisc); err != nil {
		log.Fatalln("pfring ring creation error:", err)
	}
	if err = ring.SetSocketMode(ReadOnly); err != nil {
		log.Fatalln("pfring SetSocketMode error:", err)
	} else if err = ring.Enable(); err != nil {
		log.Fatalln("pfring Enable error:", err)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_, ci, _ := ring.ZeroCopyReadPacketData()
		b.SetBytes(int64(ci.CaptureLength))
	}
}

func BenchmarkPfringReadTo(b *testing.B) {
	var ring *Ring
	var err error
	if ring, err = NewRing(*iface, 65536, FlagPromisc); err != nil {
		log.Fatalln("pfring ring creation error:", err)
	}
	if err = ring.SetSocketMode(ReadOnly); err != nil {
		log.Fatalln("pfring SetSocketMode error:", err)
	} else if err = ring.Enable(); err != nil {
		log.Fatalln("pfring Enable error:", err)
	}
	buffer := make([]byte, 65536*2)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		ci, _ := ring.ReadPacketDataTo(buffer)
		b.SetBytes(int64(ci.CaptureLength))
	}
}
