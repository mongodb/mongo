// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package tcpreader

import (
	"bytes"
	"fmt"
	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"github.com/google/gopacket/tcpassembly"
	"io"
	"net"
	"testing"
)

var netFlow gopacket.Flow

func init() {
	netFlow, _ = gopacket.FlowFromEndpoints(
		layers.NewIPEndpoint(net.IP{1, 2, 3, 4}),
		layers.NewIPEndpoint(net.IP{5, 6, 7, 8}))
}

type readReturn struct {
	data []byte
	err  error
}
type readSequence struct {
	in   []layers.TCP
	want []readReturn
}
type testReaderFactory struct {
	lossErrors bool
	readSize   int
	ReaderStream
	output chan []byte
}

func (t *testReaderFactory) New(a, b gopacket.Flow) tcpassembly.Stream {
	return &t.ReaderStream
}

func testReadSequence(t *testing.T, lossErrors bool, readSize int, seq readSequence) {
	f := &testReaderFactory{ReaderStream: NewReaderStream()}
	f.ReaderStream.LossErrors = lossErrors
	p := tcpassembly.NewStreamPool(f)
	a := tcpassembly.NewAssembler(p)
	buf := make([]byte, readSize)
	go func() {
		for i, test := range seq.in {
			fmt.Println("Assembling", i)
			a.Assemble(netFlow, &test)
			fmt.Println("Assembly done")
		}
	}()
	for i, test := range seq.want {
		fmt.Println("Waiting for read", i)
		n, err := f.Read(buf[:])
		fmt.Println("Got read")
		if n != len(test.data) {
			t.Errorf("test %d want %d bytes, got %d bytes", i, len(test.data), n)
		} else if err != test.err {
			t.Errorf("test %d want err %v, got err %v", i, test.err, err)
		} else if !bytes.Equal(buf[:n], test.data) {
			t.Errorf("test %d\nwant: %v\n got: %v\n", i, test.data, buf[:n])
		}
	}
	fmt.Println("All done reads")
}

func TestRead(t *testing.T) {
	testReadSequence(t, false, 10, readSequence{
		in: []layers.TCP{
			{
				SYN:       true,
				SrcPort:   1,
				DstPort:   2,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
			},
			{
				FIN:     true,
				SrcPort: 1,
				DstPort: 2,
				Seq:     1004,
			},
		},
		want: []readReturn{
			{data: []byte{1, 2, 3}},
			{err: io.EOF},
		},
	})
}

func TestReadSmallChunks(t *testing.T) {
	testReadSequence(t, false, 2, readSequence{
		in: []layers.TCP{
			{
				SYN:       true,
				SrcPort:   1,
				DstPort:   2,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
			},
			{
				FIN:     true,
				SrcPort: 1,
				DstPort: 2,
				Seq:     1004,
			},
		},
		want: []readReturn{
			{data: []byte{1, 2}},
			{data: []byte{3}},
			{err: io.EOF},
		},
	})
}

func ExampleDiscardBytesToEOF() {
	b := bytes.NewBuffer([]byte{1, 2, 3, 4, 5})
	fmt.Println(DiscardBytesToEOF(b))
	// Output:
	// 5
}
