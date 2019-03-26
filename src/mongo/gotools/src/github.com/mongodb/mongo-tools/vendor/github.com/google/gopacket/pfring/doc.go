// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

/*Package pfring wraps the PF_RING C library for Go.

PF_RING is a high-performance packet capture library written by ntop.org (see
http://www.ntop.org/products/pf_ring/).  This library allows you to utilize the
PF_RING library with gopacket to read packet data and decode it.

This package is meant to be used with its parent,
http://github.com/google/gopacket, although it can also be used independently
if you just want to get packet data from the wire.

Simple Example

This is probably the simplest code you can use to start getting packets through
pfring:

 if ring, err := pfring.NewRing("eth0", 65536, pfring.FlagPromisc); err != nil {
   panic(err)
 } else if err := ring.SetBPFFilter("tcp and port 80"); err != nil {  // optional
   panic(err)
 } else if err := ring.Enable(); err != nil { // Must do this!, or you get no packets!
   panic(err)
 } else {
   packetSource := gopacket.NewPacketSource(ring, layers.LinkTypeEthernet)
	 for packet := range packetSource.Packets() {
     handlePacket(packet)  // Do something with a packet here.
   }
 }

Pfring Tweaks

PF_RING has a ton of optimizations and tweaks to make sure you get just the
packets you want.  For example, if you're only using pfring to read packets,
consider running:

 ring.SetSocketMode(pfring.ReadOnly)

If you only care about packets received on your interface (not those transmitted
by the interface), you can run:

 ring.SetDirection(pfring.ReceiveOnly)

Pfring Clusters

PF_RING has an idea of 'clusters', where multiple applications can all read from
the same cluster, and PF_RING will multiplex packets over that cluster such that
only one application receives each packet.  We won't discuss this mechanism in
too much more detail (see the ntop.org docs for more info), but here's how to
utilize this with the pfring go library:

 ring.SetCluster(1, pfring.ClusterPerFlow5Tuple)
*/
package pfring
