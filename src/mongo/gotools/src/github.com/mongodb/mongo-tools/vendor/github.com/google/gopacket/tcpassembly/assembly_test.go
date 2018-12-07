// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package tcpassembly

import (
	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"net"
	"reflect"
	"testing"
	"time"
)

var netFlow gopacket.Flow

func init() {
	netFlow, _ = gopacket.FlowFromEndpoints(
		layers.NewIPEndpoint(net.IP{1, 2, 3, 4}),
		layers.NewIPEndpoint(net.IP{5, 6, 7, 8}))
}

type testSequence struct {
	in   layers.TCP
	want []Reassembly
}

type testFactory struct {
	reassembly []Reassembly
}

func (t *testFactory) New(a, b gopacket.Flow) Stream {
	return t
}
func (t *testFactory) Reassembled(r []Reassembly) {
	t.reassembly = r
	for i := 0; i < len(r); i++ {
		t.reassembly[i].Seen = time.Time{}
	}
}
func (t *testFactory) ReassemblyComplete() {
}

func test(t *testing.T, s []testSequence) {
	fact := &testFactory{}
	p := NewStreamPool(fact)
	a := NewAssembler(p)
	a.MaxBufferedPagesPerConnection = 4
	for i, test := range s {
		fact.reassembly = []Reassembly{}
		a.Assemble(netFlow, &test.in)
		if !reflect.DeepEqual(fact.reassembly, test.want) {
			t.Fatalf("test %v:\nwant: %v\n got: %v\n", i, test.want, fact.reassembly)
		}
	}
}

func TestReorder(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1001,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1004,
				BaseLayer: layers.BaseLayer{Payload: []byte{2, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{4, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{3, 2, 3}},
			},
			want: []Reassembly{
				Reassembly{
					Skip:  -1,
					Bytes: []byte{1, 2, 3},
				},
				Reassembly{
					Bytes: []byte{2, 2, 3},
				},
				Reassembly{
					Bytes: []byte{3, 2, 3},
				},
				Reassembly{
					Bytes: []byte{4, 2, 3},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1016,
				BaseLayer: layers.BaseLayer{Payload: []byte{2, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1019,
				BaseLayer: layers.BaseLayer{Payload: []byte{3, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1013,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
			},
			want: []Reassembly{
				Reassembly{
					Bytes: []byte{1, 2, 3},
				},
				Reassembly{
					Bytes: []byte{2, 2, 3},
				},
				Reassembly{
					Bytes: []byte{3, 2, 3},
				},
			},
		},
	})
}

func TestMaxPerSkip(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1000,
				SYN:       true,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{3, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{4, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1013,
				BaseLayer: layers.BaseLayer{Payload: []byte{5, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1016,
				BaseLayer: layers.BaseLayer{Payload: []byte{6, 2, 3}},
			},
			want: []Reassembly{
				Reassembly{
					Skip:  3,
					Bytes: []byte{3, 2, 3},
				},
				Reassembly{
					Bytes: []byte{4, 2, 3},
				},
				Reassembly{
					Bytes: []byte{5, 2, 3},
				},
				Reassembly{
					Bytes: []byte{6, 2, 3},
				},
			},
		},
	})
}

func TestReorderFast(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{3, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1004,
				BaseLayer: layers.BaseLayer{Payload: []byte{2, 2, 3}},
			},
			want: []Reassembly{
				Reassembly{
					Bytes: []byte{2, 2, 3},
				},
				Reassembly{
					Bytes: []byte{3, 2, 3},
				},
			},
		},
	})
}

func TestOverlap(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{7, 8, 9, 0, 1, 2, 3, 4, 5}},
			},
			want: []Reassembly{
				Reassembly{
					Bytes: []byte{1, 2, 3, 4, 5},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{0, 1, 2, 3, 4, 5, 6, 7}},
			},
			want: []Reassembly{
				Reassembly{
					Bytes: []byte{6, 7},
				},
			},
		},
	})
}

func TestBufferedOverlap(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{7, 8, 9, 0, 1, 2, 3, 4, 5}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{0, 1, 2, 3, 4, 5, 6, 7}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0},
				},
				Reassembly{
					Bytes: []byte{1, 2, 3, 4, 5},
				},
				Reassembly{
					Bytes: []byte{6, 7},
				},
			},
		},
	})
}

func TestOverrun1(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       0xFFFFFFFF,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       10,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4}},
			},
			want: []Reassembly{
				Reassembly{
					Bytes: []byte{1, 2, 3, 4},
				},
			},
		},
	})
}

func TestOverrun2(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       10,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       0xFFFFFFFF,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0},
				},
				Reassembly{
					Bytes: []byte{1, 2, 3, 4},
				},
			},
		},
	})
}

func TestCacheLargePacket(t *testing.T) {
	data := make([]byte, pageBytes*3)
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1001,
				BaseLayer: layers.BaseLayer{Payload: data},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1000,
				SYN:       true,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{},
				},
				Reassembly{
					Bytes: data[:pageBytes],
				},
				Reassembly{
					Bytes: data[pageBytes : pageBytes*2],
				},
				Reassembly{
					Bytes: data[pageBytes*2 : pageBytes*3],
				},
			},
		},
	})
}

func BenchmarkSingleStream(b *testing.B) {
	t := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		SYN:       true,
		Seq:       1000,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	a := NewAssembler(NewStreamPool(&testFactory{}))
	for i := 0; i < b.N; i++ {
		a.Assemble(netFlow, &t)
		if t.SYN {
			t.SYN = false
			t.Seq++
		}
		t.Seq += 10
	}
}

func BenchmarkSingleStreamSkips(b *testing.B) {
	t := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		SYN:       true,
		Seq:       1000,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	a := NewAssembler(NewStreamPool(&testFactory{}))
	skipped := false
	for i := 0; i < b.N; i++ {
		if i%10 == 9 {
			t.Seq += 10
			skipped = true
		} else if skipped {
			t.Seq -= 20
		}
		a.Assemble(netFlow, &t)
		if t.SYN {
			t.SYN = false
			t.Seq++
		}
		t.Seq += 10
		if skipped {
			t.Seq += 10
			skipped = false
		}
	}
}

func BenchmarkSingleStreamLoss(b *testing.B) {
	t := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		SYN:       true,
		Seq:       1000,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	a := NewAssembler(NewStreamPool(&testFactory{}))
	for i := 0; i < b.N; i++ {
		a.Assemble(netFlow, &t)
		t.SYN = false
		t.Seq += 11
	}
}

func BenchmarkMultiStreamGrow(b *testing.B) {
	t := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		Seq:       0,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	a := NewAssembler(NewStreamPool(&testFactory{}))
	for i := 0; i < b.N; i++ {
		t.SrcPort = layers.TCPPort(i)
		a.Assemble(netFlow, &t)
		t.Seq += 10
	}
}

func BenchmarkMultiStreamConn(b *testing.B) {
	t := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		Seq:       0,
		SYN:       true,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	a := NewAssembler(NewStreamPool(&testFactory{}))
	for i := 0; i < b.N; i++ {
		t.SrcPort = layers.TCPPort(i)
		a.Assemble(netFlow, &t)
		if i%65536 == 65535 {
			if t.SYN {
				t.SYN = false
				t.Seq += 1
			}
			t.Seq += 10
		}
	}
}
