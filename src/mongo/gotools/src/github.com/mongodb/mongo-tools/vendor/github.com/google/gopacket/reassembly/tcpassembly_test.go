// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package reassembly

import (
	"encoding/hex"
	"fmt"
	"net"
	"reflect"
	"runtime"
	"testing"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

var netFlow gopacket.Flow

var testDebug = false

func init() {
	netFlow, _ = gopacket.FlowFromEndpoints(
		layers.NewIPEndpoint(net.IP{1, 2, 3, 4}),
		layers.NewIPEndpoint(net.IP{5, 6, 7, 8}))
}

type Reassembly struct {
	Bytes []byte
	Start bool
	End   bool
	Skip  int
}

type testSequence struct {
	in   layers.TCP
	want []Reassembly
}

/* For benchmark: do nothing */
type testFactoryBench struct {
}

func (t *testFactoryBench) New(a, b gopacket.Flow, tcp *layers.TCP, ac AssemblerContext) Stream {
	return t
}
func (t *testFactoryBench) Accept(tcp *layers.TCP, ci gopacket.CaptureInfo, dir TCPFlowDirection, seq Sequence, start *bool, ac AssemblerContext) bool {
	return true
}
func (t *testFactoryBench) ReassembledSG(sg ScatterGather, ac AssemblerContext) {
}
func (t *testFactoryBench) ReassemblyComplete(ac AssemblerContext) bool {
	return true
}

/* For tests: append bytes */
type testFactory struct {
	reassembly []Reassembly
}

func (t *testFactory) New(a, b gopacket.Flow, tcp *layers.TCP, ac AssemblerContext) Stream {
	return t
}
func (t *testFactory) Reassembled(r []Reassembly) {
	t.reassembly = r
	for i := 0; i < len(r); i++ {
		//t.reassembly[i].Seen = time.Time{}
	}
}
func (t *testFactory) ReassembledSG(sg ScatterGather, ac AssemblerContext) {
	_, start, end, skip := sg.Info()
	l, _ := sg.Lengths()
	t.reassembly = append(t.reassembly, Reassembly{
		Bytes: sg.Fetch(l),
		Skip:  skip,
		Start: start,
		End:   end,
	})
}

func (t *testFactory) ReassemblyComplete(ac AssemblerContext) bool {
	return true
}

func (t *testFactory) Accept(tcp *layers.TCP, ci gopacket.CaptureInfo, dir TCPFlowDirection, seq Sequence, start *bool, ac AssemblerContext) bool {
	return true
}

/* For memory checks: counts bytes */
type testMemoryFactory struct {
	bytes int
}

func (tf *testMemoryFactory) New(a, b gopacket.Flow, tcp *layers.TCP, ac AssemblerContext) Stream {
	return tf
}
func (tf *testMemoryFactory) Accept(tcp *layers.TCP, ci gopacket.CaptureInfo, dir TCPFlowDirection, seq Sequence, start *bool, ac AssemblerContext) bool {
	return true
}
func (tf *testMemoryFactory) ReassembledSG(sg ScatterGather, ac AssemblerContext) {
	bytes, _ := sg.Lengths()
	tf.bytes += bytes
}
func (tf *testMemoryFactory) ReassemblyComplete(ac AssemblerContext) bool {
	return true
}

/*
 * Tests
 */

func test(t *testing.T, s []testSequence) {
	fact := &testFactory{}
	p := NewStreamPool(fact)
	a := NewAssembler(p)
	a.MaxBufferedPagesPerConnection = 4
	for i, test := range s {
		fact.reassembly = []Reassembly{}
		if testDebug {
			fmt.Printf("#### test: #%d: sending:%s\n", i, hex.EncodeToString(test.in.BaseLayer.Payload))
		}
		a.Assemble(netFlow, &test.in)
		final := []Reassembly{}
		if len(test.want) > 0 {
			final = append(final, Reassembly{})
			for _, w := range test.want {
				final[0].Bytes = append(final[0].Bytes, w.Bytes...)
				if w.End {
					final[0].End = true
				}
				if w.Start {
					final[0].Start = true
				}
				if w.Skip != 0 {
					final[0].Skip = w.Skip
				}
			}
		}
		if !reflect.DeepEqual(fact.reassembly, final) {
			t.Fatalf("test %v:\nwant: %v\n got: %v\n", i, final, fact.reassembly)
		}
		if testDebug {
			fmt.Printf("test %v passing...(%v)\n", i, final)
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
				BaseLayer: layers.BaseLayer{Payload: []byte{4, 5, 6}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{10, 11, 12}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{7, 8, 9}},
			},
			want: []Reassembly{
				Reassembly{
					Skip:  -1,
					Bytes: []byte{1, 2, 3},
				},
				Reassembly{
					Bytes: []byte{4, 5, 6},
				},
				Reassembly{
					Bytes: []byte{7, 8, 9},
				},
				Reassembly{
					Bytes: []byte{10, 11, 12},
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

func TestBufferedOverlap1(t *testing.T) {
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

func TestBufferedOverlapCase6(t *testing.T) {
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
				BaseLayer: layers.BaseLayer{Payload: []byte{10, 11, 12, 13, 14}},
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
					Bytes: []byte{11, 12, 13, 14, 5},
				},
			},
		},
	})
}

func TestBufferedOverlapExisting(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1000,
				SYN:       true,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3, 4, 5, 6, 7},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1005,
				BaseLayer: layers.BaseLayer{Payload: []byte{5, 6, 7, 8, 9, 10}},
			},
			want: []Reassembly{
				Reassembly{
					Bytes: []byte{8, 9, 10},
				},
			},
		},
	})
}

func TestBufferedOverlapReemit(t *testing.T) {
	test(t, []testSequence{
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1000,
				SYN:       true,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7}},
			},
			want: []Reassembly{
				Reassembly{
					Start: true,
					Bytes: []byte{1, 2, 3, 4, 5, 6, 7},
				},
			},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1003,
				BaseLayer: layers.BaseLayer{Payload: []byte{3, 4, 5}},
			},
			want: []Reassembly{},
		},
	})
}

func TestReorderRetransmission2(t *testing.T) {
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
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{2, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{2, 2, 3}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{10, 11}},
			},
			want: []Reassembly{},
		},
		{
			in: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1004,
				BaseLayer: layers.BaseLayer{Payload: []byte{6, 6, 6, 2, 2}},
			},
			want: []Reassembly{
				Reassembly{
					Skip:  -1,
					Bytes: []byte{1, 2, 3},
				},
				Reassembly{
					Bytes: []byte{6, 6, 6},
				},
				Reassembly{
					Bytes: []byte{2, 2, 3},
				},
				Reassembly{
					Bytes: []byte{10, 11},
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

func testFlush(t *testing.T, s []testSequence, delay time.Duration, flushInterval time.Duration) {
	fact := &testFactory{}
	p := NewStreamPool(fact)
	a := NewAssembler(p)
	a.MaxBufferedPagesPerConnection = 10
	port := layers.TCPPort(0)

	for i, test := range s {
		fact.reassembly = []Reassembly{}
		if testDebug {
			fmt.Printf("#### test: #%d: sending:%s\n", i, hex.EncodeToString(test.in.BaseLayer.Payload))
		}

		flow := netFlow
		if port == 0 {
			port = test.in.SrcPort
		}
		if port != test.in.SrcPort {
			flow = flow.Reverse()
		}
		a.Assemble(flow, &test.in)
		time.Sleep(delay)
		a.FlushCloseOlderThan(time.Now().Add(-1 * flushInterval))

		final := []Reassembly{}
		if len(test.want) > 0 {
			final = append(final, Reassembly{})
			for _, w := range test.want {
				final[0].Bytes = append(final[0].Bytes, w.Bytes...)
				if w.End {
					final[0].End = true
				}
				if w.Start {
					final[0].Start = true
				}
				if w.Skip != 0 {
					final[0].Skip = w.Skip
				}
			}
		}

		if !reflect.DeepEqual(fact.reassembly, final) {
			t.Errorf("test %v:\nwant: %v\n got: %v\n", i, final, fact.reassembly)
		}

		if testDebug {
			fmt.Printf("test %v passing...(%v)\n", i, final)
		}
	}
}

func TestFlush(t *testing.T) {
	for _, test := range []struct {
		seq                   []testSequence
		delay, flushOlderThan time.Duration
	}{
		{
			seq: []testSequence{
				{
					in: layers.TCP{
						SrcPort:   1,
						DstPort:   2,
						Seq:       1001,
						BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
					},
					want: []Reassembly{
						// flushed after flush interval.
						Reassembly{
							Skip:  -1,
							Bytes: []byte{1, 2, 3},
						},
					},
				},
				{
					in: layers.TCP{
						SrcPort:   1,
						DstPort:   2,
						Seq:       1010,
						BaseLayer: layers.BaseLayer{Payload: []byte{4, 5, 6, 7}},
					},
					want: []Reassembly{
						// flushed after flush interval.
						Reassembly{
							Skip:  -1,
							Bytes: []byte{4, 5, 6, 7},
						},
					},
				},
			},
			delay:          time.Millisecond * 50,
			flushOlderThan: time.Millisecond * 40,
		},
		{
			// two way stream.
			seq: []testSequence{
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
						SrcPort:   2,
						DstPort:   1,
						Seq:       890,
						BaseLayer: layers.BaseLayer{Payload: []byte{11, 22, 33}},
					},
					want: []Reassembly{
						// First half is flushed after flush interval.
						Reassembly{
							Skip:  -1,
							Bytes: []byte{1, 2, 3},
						},
					},
				},
				{
					in: layers.TCP{
						SrcPort:   2,
						DstPort:   1,
						Seq:       893,
						BaseLayer: layers.BaseLayer{Payload: []byte{44, 55, 66, 77}},
					},
					want: []Reassembly{
						// continues data is flushed.
						Reassembly{
							Skip:  -1,
							Bytes: []byte{11, 22, 33, 44, 55, 66, 77},
						},
					},
				},
				{
					in: layers.TCP{
						SrcPort:   1,
						DstPort:   2,
						Seq:       1004,
						BaseLayer: layers.BaseLayer{Payload: []byte{8, 9}},
					},
					want: []Reassembly{
						Reassembly{
							// Should be flushed because is continues.
							Bytes: []byte{8, 9},
						},
					},
				},
			},
			delay:          time.Millisecond * 50,
			flushOlderThan: time.Millisecond * 99,
		},
	} {
		testFlush(t, test.seq, test.delay, test.flushOlderThan)
	}
}

/*
 * Keep
 */
type testKeepFactory struct {
	keep    int
	bytes   []byte
	skipped int
	t       *testing.T
}

func (tkf *testKeepFactory) New(a, b gopacket.Flow, tcp *layers.TCP, ac AssemblerContext) Stream {
	return tkf
}
func (tkf *testKeepFactory) ReassembledSG(sg ScatterGather, ac AssemblerContext) {
	l, _ := sg.Lengths()
	_, _, _, tkf.skipped = sg.Info()
	tkf.bytes = sg.Fetch(l)
	sg.KeepFrom(tkf.keep)
}
func (tkf *testKeepFactory) ReassemblyComplete(ac AssemblerContext) bool {
	return true
}

func (tkf *testKeepFactory) Accept(tcp *layers.TCP, ci gopacket.CaptureInfo, dir TCPFlowDirection, seq Sequence, start *bool, ac AssemblerContext) bool {
	return true
}

type testKeepSequence struct {
	tcp     layers.TCP
	keep    int
	want    []byte
	skipped int
}

func testKeep(t *testing.T, s []testKeepSequence) {
	fact := &testKeepFactory{t: t}
	p := NewStreamPool(fact)
	a := NewAssembler(p)
	a.MaxBufferedPagesPerConnection = 4
	port := layers.TCPPort(0)
	for i, test := range s {
		// Fake some values according to ports
		flow := netFlow
		dir := TCPDirClientToServer
		if port == 0 {
			port = test.tcp.SrcPort
		}
		if port != test.tcp.SrcPort {
			dir = dir.Reverse()
			flow = flow.Reverse()
		}
		test.tcp.SetInternalPortsForTesting()
		fact.keep = test.keep
		fact.bytes = []byte{}
		if testDebug {
			fmt.Printf("#### testKeep: #%d: sending:%s\n", i, hex.EncodeToString(test.tcp.BaseLayer.Payload))
		}
		a.Assemble(flow, &test.tcp)
		if !reflect.DeepEqual(fact.bytes, test.want) {
			t.Fatalf("#%d: invalid bytes: got %v, expected %v", i, fact.bytes, test.want)
		}
		if fact.skipped != test.skipped {
			t.Fatalf("#%d: expecting %d skipped bytes, got %d", i, test.skipped, fact.skipped)
		}
		if testDebug {
			fmt.Printf("#### testKeep: #%d: bytes: %s\n", i, hex.EncodeToString(fact.bytes))
		}
	}
}

func TestKeepSimpleOnBoundary(t *testing.T) {
	testKeep(t, []testKeepSequence{
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
			},
			keep: 0,
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{7, 8, 9, 0, 1, 2, 3, 4, 5}},
			},
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5},
		},
	})
}

func TestKeepSimpleNotBoundaryLive(t *testing.T) {
	testKeep(t, []testKeepSequence{
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
			},
			keep: 1,
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{7, 8, 9, 0, 1, 2, 3, 4, 5}},
			},
			want: []byte{2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5},
		},
	})
}

func TestKeepSimpleNotBoundaryAlreadyKept(t *testing.T) {
	testKeep(t, []testKeepSequence{
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0x10}},
			},
			keep: 0, // 1→10
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0x10},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{7, 8, 9, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15}},
			},
			keep: 11, // 12→15
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1016,
				BaseLayer: layers.BaseLayer{Payload: []byte{0x16, 0x17, 0x18}},
			},
			want: []byte{0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18},
		},
	})
}

func TestKeepLonger(t *testing.T) {
	testKeep(t, []testKeepSequence{
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
			},
			keep: 0,
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1007,
				BaseLayer: layers.BaseLayer{Payload: []byte{7, 8, 9, 10, 11, 12, 13, 14, 15}},
			},
			keep: 0,
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{10, 11, 12, 13, 14, 15, 16, 17}},
			},
			want: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
		},
	})
}

func TestKeepWithFlush(t *testing.T) {
	testKeep(t, []testKeepSequence{
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				SYN:       true,
				Seq:       1000,
				BaseLayer: layers.BaseLayer{Payload: []byte{1}},
			},
			keep: 1,
			want: []byte{1},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1003,
				BaseLayer: layers.BaseLayer{Payload: []byte{3}},
			},
			keep: 0,
			want: []byte{},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1004,
				BaseLayer: layers.BaseLayer{Payload: []byte{4}},
			},
			keep: 0,
			want: []byte{},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1006,
				BaseLayer: layers.BaseLayer{Payload: []byte{6}},
			},
			keep: 0,
			want: []byte{},
		},
		// Exceeding 4 pages: flushing first continuous pages
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1008,
				BaseLayer: layers.BaseLayer{Payload: []byte{8}},
			},
			keep:    0,
			skipped: 1,
			want:    []byte{3, 4},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1010,
				BaseLayer: layers.BaseLayer{Payload: []byte{10}},
			},
			keep:    0,
			skipped: 1,
			want:    []byte{6},
		},
		{
			tcp: layers.TCP{
				SrcPort:   1,
				DstPort:   2,
				Seq:       1012,
				BaseLayer: layers.BaseLayer{Payload: []byte{12}},
			},
			keep:    0,
			skipped: 1,
			want:    []byte{8},
		},
	})
}

/*
 * FSM tests
 */
/* For FSM: bump nb on accepted packet */
type testFSMFactory struct {
	nb  int
	fsm TCPSimpleFSM
}

func (t *testFSMFactory) New(a, b gopacket.Flow, tcp *layers.TCP, ac AssemblerContext) Stream {
	return t
}
func (t *testFSMFactory) ReassembledSG(sg ScatterGather, ac AssemblerContext) {
}
func (t *testFSMFactory) ReassemblyComplete(ac AssemblerContext) bool {
	return false
}

func (t *testFSMFactory) Accept(tcp *layers.TCP, ci gopacket.CaptureInfo, dir TCPFlowDirection, seq Sequence, start *bool, ac AssemblerContext) bool {
	ok := t.fsm.CheckState(tcp, dir)
	if ok {
		t.nb++
	}
	return ok
}

type testFSMSequence struct {
	tcp layers.TCP
	ci  gopacket.CaptureInfo
	nb  int
}

func (seq *testFSMSequence) GetCaptureInfo() gopacket.CaptureInfo {
	return seq.ci
}

func testFSM(t *testing.T, s []testFSMSequence) {
	fact := &testFSMFactory{}
	p := NewStreamPool(fact)
	a := NewAssembler(p)
	//a.MaxBufferedPagesPerConnection = 4
	fact.nb = 0
	port := layers.TCPPort(0)
	for i, test := range s {
		// Fake some values according to ports
		flow := netFlow
		dir := TCPDirClientToServer
		if port == 0 {
			port = test.tcp.SrcPort
		}
		if port != test.tcp.SrcPort {
			dir = dir.Reverse()
			flow = flow.Reverse()
		}
		test.tcp.SetInternalPortsForTesting()
		a.AssembleWithContext(flow, &test.tcp, &test)
		if fact.nb != test.nb {
			t.Fatalf("#%d: packet rejected: got %d, expected %d", i, fact.nb, test.nb)
		}
	}
}

func TestFSMnormalFlow(t *testing.T) {
	testFSM(t, []testFSMSequence{
		{
			tcp: layers.TCP{
				SYN:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511116,
				Ack:       0,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 566690000),
			},
			nb: 1,
		},
		{
			tcp: layers.TCP{
				SYN:       true,
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787765,
				Ack:       374511117,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590332000),
			},
			nb: 2,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590346000),
			},
			nb: 3,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{0, 31, 104, 196, 0, 32, 0, 1, 0, 0, 0, 0, 0, 1, 2, 85, 83, 0, 0, 6, 0, 1, 0, 0, 41, 16, 0, 0, 0, 128, 0, 0, 0}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590387000),
			},
			nb: 4,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787766,
				Ack:       374511150,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 613687000),
			},
			nb: 5,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787766,
				Ack:       374511150,
				BaseLayer: layers.BaseLayer{Payload: []byte{8, 133, 104, 196, 132, 0, 0, 1, 0, 2, 0, 7, 0, 19, 2, 85, 83, 0, 0, 6, 0, 1, 2, 117, 115, 0, 0, 6, 0, 1, 0, 0, 3, 132, 0, 54, 1, 97, 5, 99, 99, 116, 108, 100, 192, 20, 10, 104, 111, 115, 116, 109, 97, 115, 116, 101, 114, 7, 110, 101, 117, 115, 116, 97, 114, 3, 98, 105, 122, 0, 120, 18, 40, 205, 0, 0, 3, 132, 0, 0, 3, 132, 0, 9, 58, 128, 0, 1, 81, 128, 192, 20, 0, 46, 0, 1, 0, 0, 3, 132, 0, 150, 0, 6, 5, 1, 0, 0, 3, 132, 85, 138, 90, 146, 85, 98, 191, 130, 27, 78, 2, 117, 115, 0, 69, 13, 35, 189, 141, 225, 107, 238, 108, 182, 207, 44, 105, 31, 212, 103, 32, 93, 217, 108, 20, 231, 188, 28, 241, 237, 104, 182, 117, 121, 195, 112, 64, 96, 237, 248, 6, 181, 186, 96, 60, 6, 18, 29, 188, 96, 201, 140, 251, 61, 71, 177, 108, 156, 9, 83, 125, 172, 188, 75, 81, 67, 218, 55, 93, 131, 243, 15, 190, 75, 4, 165, 226, 124, 49, 67, 142, 131, 239, 240, 76, 225, 10, 242, 68, 88, 240, 200, 27, 97, 102, 73, 92, 73, 133, 170, 175, 198, 99, 109, 90, 16, 162, 101, 95, 96, 102, 250, 91, 74, 80, 3, 87, 167, 50, 230, 9, 213, 7, 222, 197, 87, 183, 190, 148, 247, 207, 204, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 10, 1, 102, 5, 99, 99, 116, 108, 100, 192, 12, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 97, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 98, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 99, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 101, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 107, 193, 8, 192, 118, 0, 46, 0, 1, 0, 7, 233, 0, 0, 150, 0, 2, 5, 1, 0, 7, 233, 0, 85, 127, 33, 92, 85, 87, 134, 98, 27, 78, 2, 117, 115, 0, 19, 227, 175, 75, 88, 245, 164, 158, 150, 198, 57, 253, 150, 179, 161, 52, 24, 56, 229, 176, 175, 40, 45, 232, 188, 171, 131, 197, 107, 125, 218, 192, 78, 221, 146, 33, 114, 55, 43, 12, 131, 213, 51, 98, 37, 2, 102, 161, 232, 115, 177, 210, 51, 169, 215, 133, 56, 190, 91, 75, 8, 222, 231, 202, 139, 28, 187, 249, 72, 21, 23, 56, 63, 72, 126, 142, 242, 195, 242, 64, 208, 134, 100, 157, 197, 159, 43, 148, 20, 70, 117, 152, 159, 35, 200, 220, 49, 234, 173, 210, 91, 34, 210, 192, 7, 197, 112, 117, 208, 234, 42, 49, 133, 237, 197, 14, 244, 149, 191, 142, 36, 252, 42, 48, 182, 189, 9, 68, 1, 65, 5, 67, 67, 84, 76, 68, 193, 126, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 124, 70, 1, 66, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 125, 70, 194, 26, 0, 28, 0, 1, 0, 0, 28, 32, 0, 16, 32, 1, 5, 3, 209, 174, 255, 255, 255, 255, 255, 255, 255, 255, 255, 126, 1, 67, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 127, 70, 1, 69, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 126, 70, 1, 70, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 209, 173, 58, 70, 194, 108, 0, 28, 0, 1, 0, 0, 28, 32, 0, 16, 32, 1, 5, 0, 54, 130, 0, 0, 0, 0, 0, 0, 0, 0, 0, 17, 1, 75, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 128, 70, 194, 154, 0, 28, 0, 1, 0, 0, 28, 32, 0, 16, 32, 1, 5, 3, 226, 57, 0, 0, 0, 0, 0, 0, 0, 3, 0, 1, 194, 2, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 112, 230, 49, 85, 73, 83, 2, 27, 78, 2, 117, 115, 0, 82, 36, 11, 141, 74, 85, 70, 98, 179, 63, 173, 83, 8, 70, 155, 41, 102, 166, 140, 62, 71, 178, 130, 38, 171, 200, 180, 68, 2, 215, 45, 6, 43, 59, 171, 146, 223, 215, 9, 77, 5, 104, 167, 42, 237, 170, 30, 114, 205, 129, 59, 225, 152, 224, 79, 1, 65, 68, 208, 153, 121, 237, 199, 87, 2, 251, 100, 105, 59, 24, 73, 226, 169, 121, 250, 91, 41, 124, 14, 23, 135, 52, 2, 86, 72, 224, 100, 135, 70, 216, 16, 107, 84, 59, 13, 168, 58, 187, 54, 98, 230, 167, 246, 42, 46, 156, 206, 238, 120, 199, 25, 144, 98, 249, 70, 162, 34, 43, 145, 114, 186, 233, 47, 42, 75, 95, 152, 235, 194, 26, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 112, 190, 140, 85, 73, 36, 78, 27, 78, 2, 117, 115, 0, 160, 95, 100, 37, 167, 82, 93, 165, 126, 247, 147, 173, 238, 154, 206, 174, 96, 175, 209, 7, 8, 169, 171, 223, 29, 201, 161, 177, 98, 54, 94, 62, 70, 127, 142, 109, 206, 42, 179, 109, 156, 160, 156, 20, 59, 24, 147, 164, 13, 121, 192, 84, 157, 26, 56, 177, 151, 210, 7, 197, 229, 110, 60, 58, 224, 42, 77, 5, 59, 80, 216, 221, 248, 19, 66, 102, 74, 199, 238, 120, 231, 201, 187, 29, 11, 46, 195, 164, 8, 221, 128, 25, 205, 42, 247, 152, 112, 176, 14, 117, 150, 223, 245, 32, 212, 107, 4, 245, 27, 126, 224, 216, 0, 89, 106, 238, 185, 206, 44, 56, 204, 175, 7, 139, 233, 228, 127, 175, 194, 26, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 28, 5, 3, 0, 0, 28, 32, 85, 108, 217, 174, 85, 69, 70, 242, 27, 78, 2, 117, 115, 0, 172, 117, 89, 89, 73, 249, 245, 211, 100, 127, 48, 135, 224, 97, 172, 146, 128, 30, 190, 72, 199, 170, 97, 179, 136, 109, 86, 110, 235, 214, 47, 50, 115, 11, 226, 168, 56, 198, 24, 212, 205, 207, 2, 116, 104, 112, 99, 234, 236, 44, 70, 19, 19, 215, 127, 200, 162, 215, 142, 45, 135, 91, 219, 217, 86, 231, 154, 87, 222, 161, 32, 66, 196, 55, 117, 20, 186, 9, 134, 252, 249, 219, 9, 196, 128, 8, 222, 201, 131, 210, 182, 232, 142, 72, 160, 171, 95, 231, 232, 156, 28, 34, 54, 94, 73, 183, 38, 160, 123, 175, 157, 21, 163, 8, 214, 155, 172, 237, 169, 28, 15, 138, 105, 107, 251, 109, 131, 240, 194, 72, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 112, 190, 140, 85, 73, 36, 78, 27, 78, 2, 117, 115, 0, 77, 207, 197, 130, 236, 138, 192, 241, 225, 114, 8, 22, 76, 54, 43, 121, 42, 44, 9, 92, 56, 253, 224, 179, 191, 131, 40, 176, 94, 61, 33, 12, 43, 82, 156, 236, 211, 29, 187, 100, 220, 243, 24, 134, 42, 204, 46, 161, 214, 91, 68, 119, 40, 252, 53, 54, 146, 136, 196, 168, 204, 195, 131, 110, 6, 73, 16, 161, 86, 35, 150, 153, 162, 185, 227, 65, 228, 160, 203, 42, 250, 121, 14, 42, 115, 221, 232, 96, 99, 164, 230, 29, 195, 149, 85, 206, 41, 1, 252, 77, 188, 88, 8, 182, 37, 249, 6, 158, 6, 244, 158, 254, 141, 203, 6, 158, 198, 103, 130, 98, 123, 34, 245, 44, 126, 77, 24, 187, 194, 90, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 108, 194, 203, 85, 69, 51, 125, 27, 78, 2, 117, 115, 0, 86, 26, 187, 56, 252, 194, 199, 140, 229, 133, 186, 187, 20, 174, 26, 48, 212, 129, 10, 20, 167, 179, 53, 72, 176, 92, 153, 48, 146, 15, 163, 182, 80, 138, 181, 135, 98, 129, 17, 66, 55, 184, 76, 225, 72, 104, 7, 221, 40, 71, 41, 202, 246, 154, 166, 199, 74, 175, 146, 54, 25, 56, 115, 243}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 621198000),
			},
			nb: 6,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511150,
				Ack:       3465789226,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 621220000),
			},
			nb: 7,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465789226,
				Ack:       374511150,
				BaseLayer: layers.BaseLayer{Payload: []byte{153, 141, 101, 187, 110, 15, 63, 42, 81, 100, 95, 68, 241, 85, 160, 227, 3, 1, 12, 80, 166, 1, 98, 2, 44, 98, 63, 203, 70, 164, 99, 195, 23, 152, 223, 253, 208, 10, 12, 19, 66, 121, 9, 158, 205, 96, 218, 0, 80, 70, 58, 95, 41, 124, 216, 13, 122, 135, 102, 200, 181, 233, 129, 174, 194, 108, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 108, 223, 157, 85, 69, 74, 55, 27, 78, 2, 117, 115, 0, 149, 71, 215, 149, 16, 165, 115, 229, 141, 136, 187, 158, 88, 225, 131, 231, 182, 218, 235, 27, 48, 65, 244, 77, 186, 135, 72, 18, 87, 52, 180, 128, 130, 67, 75, 173, 160, 243, 104, 178, 103, 117, 96, 209, 36, 51, 108, 47, 232, 214, 254, 15, 208, 182, 218, 174, 248, 237, 88, 150, 35, 190, 239, 249, 171, 151, 9, 236, 2, 252, 255, 13, 79, 190, 147, 36, 161, 210, 202, 80, 209, 136, 167, 180, 186, 68, 246, 249, 48, 123, 46, 11, 132, 103, 132, 207, 186, 68, 110, 133, 142, 109, 194, 19, 122, 57, 203, 217, 120, 93, 67, 168, 91, 252, 87, 38, 33, 228, 229, 162, 190, 170, 23, 188, 89, 15, 241, 71, 194, 108, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 28, 5, 3, 0, 0, 28, 32, 85, 108, 217, 174, 85, 69, 70, 242, 27, 78, 2, 117, 115, 0, 206, 97, 120, 37, 255, 252, 7, 156, 162, 192, 43, 84, 105, 94, 125, 55, 13, 247, 234, 9, 25, 100, 246, 25, 77, 168, 199, 208, 187, 209, 164, 123, 234, 138, 238, 15, 86, 45, 163, 108, 162, 117, 247, 128, 3, 187, 100, 185, 193, 191, 134, 86, 161, 254, 236, 99, 66, 66, 35, 173, 91, 243, 175, 3, 175, 94, 79, 68, 246, 109, 200, 154, 209, 185, 11, 210, 50, 147, 136, 213, 158, 81, 111, 17, 149, 239, 110, 114, 25, 234, 247, 158, 233, 33, 36, 181, 66, 84, 189, 37, 207, 58, 9, 171, 143, 66, 69, 137, 192, 6, 187, 59, 16, 51, 80, 56, 89, 170, 12, 195, 69, 133, 188, 110, 171, 17, 17, 213, 194, 154, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 112, 190, 140, 85, 73, 36, 78, 27, 78, 2, 117, 115, 0, 123, 36, 154, 4, 158, 41, 96, 252, 116, 114, 16, 137, 28, 177, 206, 33, 192, 88, 89, 1, 69, 252, 206, 88, 89, 152, 210, 179, 248, 44, 202, 239, 95, 131, 126, 147, 249, 93, 57, 166, 215, 184, 211, 164, 196, 71, 170, 3, 25, 18, 177, 214, 94, 147, 181, 148, 197, 11, 171, 219, 107, 48, 105, 81, 239, 110, 249, 140, 68, 127, 193, 146, 176, 161, 246, 108, 75, 141, 205, 211, 73, 247, 125, 205, 120, 156, 82, 55, 130, 250, 26, 15, 44, 214, 91, 115, 11, 103, 22, 83, 184, 96, 107, 138, 2, 127, 168, 191, 92, 102, 137, 161, 63, 225, 134, 17, 178, 242, 11, 43, 8, 30, 164, 28, 140, 195, 83, 121, 194, 154, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 28, 5, 3, 0, 0, 28, 32, 85, 112, 190, 140, 85, 73, 36, 78, 27, 78, 2, 117, 115, 0, 189, 98, 234, 251, 237, 24, 143, 210, 30, 242, 97, 66, 50, 211, 47, 109, 110, 121, 244, 239, 89, 0, 39, 92, 218, 155, 71, 5, 23, 136, 231, 107, 95, 52, 231, 118, 253, 206, 250, 178, 209, 136, 13, 36, 36, 54, 157, 237, 35, 110, 134, 253, 80, 237, 162, 163, 38, 21, 54, 241, 240, 253, 73, 33, 191, 128, 32, 6, 198, 165, 35, 203, 244, 15, 166, 250, 159, 67, 149, 56, 19, 243, 230, 87, 6, 44, 150, 90, 79, 107, 18, 121, 112, 23, 176, 104, 50, 110, 176, 138, 250, 6, 209, 22, 41, 73, 234, 4, 124, 233, 208, 218, 236, 117, 232, 217, 10, 172, 18, 215, 143, 119, 193, 113, 10, 59, 255, 221, 0, 0, 41, 16, 0, 0, 0, 128, 0, 0, 0}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 622508000),
			},
			nb: 8,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511150,
				Ack:       3465789949,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 622531000),
			},
			nb: 9,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				FIN:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511150,
				Ack:       3465789949,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 622907000),
			},
			nb: 10,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				FIN:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465789949,
				Ack:       374511151,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 652784000),
			},
			nb: 11,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511151,
				Ack:       3465789950,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 652809000),
			},
			nb: 12,
		},
	})
}

func TestFSMearlyRST(t *testing.T) {
	testFSM(t, []testFSMSequence{
		{
			tcp: layers.TCP{
				SYN:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511116,
				Ack:       0,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 566690000),
			},
			nb: 1,
		},
		{
			tcp: layers.TCP{
				SYN:       true,
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787765,
				Ack:       374511117,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590332000),
			},
			nb: 2,
		},
		{
			tcp: layers.TCP{
				RST:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590346000),
			},
			nb: 3,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{0, 31, 104, 196, 0, 32, 0, 1, 0, 0, 0, 0, 0, 1, 2, 85, 83, 0, 0, 6, 0, 1, 0, 0, 41, 16, 0, 0, 0, 128, 0, 0, 0}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590387000),
			},
			nb: 3,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787766,
				Ack:       374511150,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 613687000),
			},
			nb: 3,
		},
	})
}

func TestFSMestablishedThenRST(t *testing.T) {
	testFSM(t, []testFSMSequence{
		{
			tcp: layers.TCP{
				SYN:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511116,
				Ack:       0,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 566690000),
			},
			nb: 1,
		},
		{
			tcp: layers.TCP{
				SYN:       true,
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787765,
				Ack:       374511117,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590332000),
			},
			nb: 2,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590346000),
			},
			nb: 3,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{0, 31, 104, 196, 0, 32, 0, 1, 0, 0, 0, 0, 0, 1, 2, 85, 83, 0, 0, 6, 0, 1, 0, 0, 41, 16, 0, 0, 0, 128, 0, 0, 0}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590387000),
			},
			nb: 4,
		},
		{
			tcp: layers.TCP{
				RST:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787766,
				Ack:       374511150,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 613687000),
			},
			nb: 5,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   53,
				DstPort:   54842,
				Seq:       3465787766,
				Ack:       374511150,
				BaseLayer: layers.BaseLayer{Payload: []byte{8, 133, 104, 196, 132, 0, 0, 1, 0, 2, 0, 7, 0, 19, 2, 85, 83, 0, 0, 6, 0, 1, 2, 117, 115, 0, 0, 6, 0, 1, 0, 0, 3, 132, 0, 54, 1, 97, 5, 99, 99, 116, 108, 100, 192, 20, 10, 104, 111, 115, 116, 109, 97, 115, 116, 101, 114, 7, 110, 101, 117, 115, 116, 97, 114, 3, 98, 105, 122, 0, 120, 18, 40, 205, 0, 0, 3, 132, 0, 0, 3, 132, 0, 9, 58, 128, 0, 1, 81, 128, 192, 20, 0, 46, 0, 1, 0, 0, 3, 132, 0, 150, 0, 6, 5, 1, 0, 0, 3, 132, 85, 138, 90, 146, 85, 98, 191, 130, 27, 78, 2, 117, 115, 0, 69, 13, 35, 189, 141, 225, 107, 238, 108, 182, 207, 44, 105, 31, 212, 103, 32, 93, 217, 108, 20, 231, 188, 28, 241, 237, 104, 182, 117, 121, 195, 112, 64, 96, 237, 248, 6, 181, 186, 96, 60, 6, 18, 29, 188, 96, 201, 140, 251, 61, 71, 177, 108, 156, 9, 83, 125, 172, 188, 75, 81, 67, 218, 55, 93, 131, 243, 15, 190, 75, 4, 165, 226, 124, 49, 67, 142, 131, 239, 240, 76, 225, 10, 242, 68, 88, 240, 200, 27, 97, 102, 73, 92, 73, 133, 170, 175, 198, 99, 109, 90, 16, 162, 101, 95, 96, 102, 250, 91, 74, 80, 3, 87, 167, 50, 230, 9, 213, 7, 222, 197, 87, 183, 190, 148, 247, 207, 204, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 10, 1, 102, 5, 99, 99, 116, 108, 100, 192, 12, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 97, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 98, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 99, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 101, 193, 8, 192, 118, 0, 2, 0, 1, 0, 7, 233, 0, 0, 4, 1, 107, 193, 8, 192, 118, 0, 46, 0, 1, 0, 7, 233, 0, 0, 150, 0, 2, 5, 1, 0, 7, 233, 0, 85, 127, 33, 92, 85, 87, 134, 98, 27, 78, 2, 117, 115, 0, 19, 227, 175, 75, 88, 245, 164, 158, 150, 198, 57, 253, 150, 179, 161, 52, 24, 56, 229, 176, 175, 40, 45, 232, 188, 171, 131, 197, 107, 125, 218, 192, 78, 221, 146, 33, 114, 55, 43, 12, 131, 213, 51, 98, 37, 2, 102, 161, 232, 115, 177, 210, 51, 169, 215, 133, 56, 190, 91, 75, 8, 222, 231, 202, 139, 28, 187, 249, 72, 21, 23, 56, 63, 72, 126, 142, 242, 195, 242, 64, 208, 134, 100, 157, 197, 159, 43, 148, 20, 70, 117, 152, 159, 35, 200, 220, 49, 234, 173, 210, 91, 34, 210, 192, 7, 197, 112, 117, 208, 234, 42, 49, 133, 237, 197, 14, 244, 149, 191, 142, 36, 252, 42, 48, 182, 189, 9, 68, 1, 65, 5, 67, 67, 84, 76, 68, 193, 126, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 124, 70, 1, 66, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 125, 70, 194, 26, 0, 28, 0, 1, 0, 0, 28, 32, 0, 16, 32, 1, 5, 3, 209, 174, 255, 255, 255, 255, 255, 255, 255, 255, 255, 126, 1, 67, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 127, 70, 1, 69, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 126, 70, 1, 70, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 209, 173, 58, 70, 194, 108, 0, 28, 0, 1, 0, 0, 28, 32, 0, 16, 32, 1, 5, 0, 54, 130, 0, 0, 0, 0, 0, 0, 0, 0, 0, 17, 1, 75, 194, 4, 0, 1, 0, 1, 0, 0, 28, 32, 0, 4, 156, 154, 128, 70, 194, 154, 0, 28, 0, 1, 0, 0, 28, 32, 0, 16, 32, 1, 5, 3, 226, 57, 0, 0, 0, 0, 0, 0, 0, 3, 0, 1, 194, 2, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 112, 230, 49, 85, 73, 83, 2, 27, 78, 2, 117, 115, 0, 82, 36, 11, 141, 74, 85, 70, 98, 179, 63, 173, 83, 8, 70, 155, 41, 102, 166, 140, 62, 71, 178, 130, 38, 171, 200, 180, 68, 2, 215, 45, 6, 43, 59, 171, 146, 223, 215, 9, 77, 5, 104, 167, 42, 237, 170, 30, 114, 205, 129, 59, 225, 152, 224, 79, 1, 65, 68, 208, 153, 121, 237, 199, 87, 2, 251, 100, 105, 59, 24, 73, 226, 169, 121, 250, 91, 41, 124, 14, 23, 135, 52, 2, 86, 72, 224, 100, 135, 70, 216, 16, 107, 84, 59, 13, 168, 58, 187, 54, 98, 230, 167, 246, 42, 46, 156, 206, 238, 120, 199, 25, 144, 98, 249, 70, 162, 34, 43, 145, 114, 186, 233, 47, 42, 75, 95, 152, 235, 194, 26, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 112, 190, 140, 85, 73, 36, 78, 27, 78, 2, 117, 115, 0, 160, 95, 100, 37, 167, 82, 93, 165, 126, 247, 147, 173, 238, 154, 206, 174, 96, 175, 209, 7, 8, 169, 171, 223, 29, 201, 161, 177, 98, 54, 94, 62, 70, 127, 142, 109, 206, 42, 179, 109, 156, 160, 156, 20, 59, 24, 147, 164, 13, 121, 192, 84, 157, 26, 56, 177, 151, 210, 7, 197, 229, 110, 60, 58, 224, 42, 77, 5, 59, 80, 216, 221, 248, 19, 66, 102, 74, 199, 238, 120, 231, 201, 187, 29, 11, 46, 195, 164, 8, 221, 128, 25, 205, 42, 247, 152, 112, 176, 14, 117, 150, 223, 245, 32, 212, 107, 4, 245, 27, 126, 224, 216, 0, 89, 106, 238, 185, 206, 44, 56, 204, 175, 7, 139, 233, 228, 127, 175, 194, 26, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 28, 5, 3, 0, 0, 28, 32, 85, 108, 217, 174, 85, 69, 70, 242, 27, 78, 2, 117, 115, 0, 172, 117, 89, 89, 73, 249, 245, 211, 100, 127, 48, 135, 224, 97, 172, 146, 128, 30, 190, 72, 199, 170, 97, 179, 136, 109, 86, 110, 235, 214, 47, 50, 115, 11, 226, 168, 56, 198, 24, 212, 205, 207, 2, 116, 104, 112, 99, 234, 236, 44, 70, 19, 19, 215, 127, 200, 162, 215, 142, 45, 135, 91, 219, 217, 86, 231, 154, 87, 222, 161, 32, 66, 196, 55, 117, 20, 186, 9, 134, 252, 249, 219, 9, 196, 128, 8, 222, 201, 131, 210, 182, 232, 142, 72, 160, 171, 95, 231, 232, 156, 28, 34, 54, 94, 73, 183, 38, 160, 123, 175, 157, 21, 163, 8, 214, 155, 172, 237, 169, 28, 15, 138, 105, 107, 251, 109, 131, 240, 194, 72, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 112, 190, 140, 85, 73, 36, 78, 27, 78, 2, 117, 115, 0, 77, 207, 197, 130, 236, 138, 192, 241, 225, 114, 8, 22, 76, 54, 43, 121, 42, 44, 9, 92, 56, 253, 224, 179, 191, 131, 40, 176, 94, 61, 33, 12, 43, 82, 156, 236, 211, 29, 187, 100, 220, 243, 24, 134, 42, 204, 46, 161, 214, 91, 68, 119, 40, 252, 53, 54, 146, 136, 196, 168, 204, 195, 131, 110, 6, 73, 16, 161, 86, 35, 150, 153, 162, 185, 227, 65, 228, 160, 203, 42, 250, 121, 14, 42, 115, 221, 232, 96, 99, 164, 230, 29, 195, 149, 85, 206, 41, 1, 252, 77, 188, 88, 8, 182, 37, 249, 6, 158, 6, 244, 158, 254, 141, 203, 6, 158, 198, 103, 130, 98, 123, 34, 245, 44, 126, 77, 24, 187, 194, 90, 0, 46, 0, 1, 0, 0, 28, 32, 0, 150, 0, 1, 5, 3, 0, 0, 28, 32, 85, 108, 194, 203, 85, 69, 51, 125, 27, 78, 2, 117, 115, 0, 86, 26, 187, 56, 252, 194, 199, 140, 229, 133, 186, 187, 20, 174, 26, 48, 212, 129, 10, 20, 167, 179, 53, 72, 176, 92, 153, 48, 146, 15, 163, 182, 80, 138, 181, 135, 98, 129, 17, 66, 55, 184, 76, 225, 72, 104, 7, 221, 40, 71, 41, 202, 246, 154, 166, 199, 74, 175, 146, 54, 25, 56, 115, 243}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 621198000),
			},
			nb: 5,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511150,
				Ack:       3465789226,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 621220000),
			},
			nb: 5,
		},
	})
}

func TestFSMmissingSYNACK(t *testing.T) {
	testFSM(t, []testFSMSequence{
		{
			tcp: layers.TCP{
				SYN:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511116,
				Ack:       0,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 566690000),
			},
			nb: 1,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590346000),
			},
			nb: 1,
		},
		{
			tcp: layers.TCP{
				ACK:       true,
				SrcPort:   54842,
				DstPort:   53,
				Seq:       374511117,
				Ack:       3465787766,
				BaseLayer: layers.BaseLayer{Payload: []byte{0, 31, 104, 196, 0, 32, 0, 1, 0, 0, 0, 0, 0, 1, 2, 85, 83, 0, 0, 6, 0, 1, 0, 0, 41, 16, 0, 0, 0, 128, 0, 0, 0}},
			},
			ci: gopacket.CaptureInfo{
				Timestamp: time.Unix(1432538521, 590387000),
			},
			nb: 1,
		},
	})
}

/*
 * Memory test
 */
func TestMemoryShrink(t *testing.T) {
	tcp := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		SYN:       true,
		Seq:       999,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	a := NewAssembler(NewStreamPool(&testFactoryBench{}))
	var before runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	run := 1050
	// Allocate > initial
	for i := 0; i < run; i++ {
		a.Assemble(netFlow, &tcp)
		if tcp.SYN {
			tcp.SYN = false
			tcp.Seq += 1 + 1
		}
		tcp.Seq += 10
	}
	var after runtime.MemStats
	a.FlushAll()
	runtime.GC()
	runtime.ReadMemStats(&after)
	if after.HeapAlloc < before.HeapAlloc {
		t.Fatalf("Nothing allocated for %d run: before: %d, after: %d", run, before.HeapAlloc, after.HeapAlloc)
	}
	before = after
	// Do ~ initial allocs+free()
	run *= 2
	for i := 0; i < run; i++ {
		a.Assemble(netFlow, &tcp)
		if i%50 == 0 {
			a.FlushAll()
		}
		tcp.Seq += 10
	}
	runtime.GC()
	runtime.ReadMemStats(&after)
	if after.HeapAlloc >= before.HeapAlloc {
		t.Fatalf("Nothing freed for %d run: before: %d, after: %d", run, before.HeapAlloc, after.HeapAlloc)
	}
}

/*
 * Benchmark tests
 */
func BenchmarkSingleStreamNo(b *testing.B) {
	t := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		SYN:       true,
		Seq:       1000,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	a := NewAssembler(NewStreamPool(&testFactoryBench{}))
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
	a := NewAssembler(NewStreamPool(&testFactoryBench{}))
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
	a := NewAssembler(NewStreamPool(&testFactoryBench{}))
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
	a := NewAssembler(NewStreamPool(&testFactoryBench{}))
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
	a := NewAssembler(NewStreamPool(&testFactoryBench{}))
	for i := 0; i < b.N; i++ {
		t.SrcPort = layers.TCPPort(i)
		a.Assemble(netFlow, &t)
		if i%65536 == 65535 {
			if t.SYN {
				t.SYN = false
				t.Seq++
			}
			t.Seq += 10
		}
	}
}

type testMemoryContext struct{}

func (t *testMemoryContext) GetCaptureInfo() gopacket.CaptureInfo {
	return gopacket.CaptureInfo{
		Timestamp: time.Unix(1432538521, 590387000),
	}
}

func TestFullyOrderedAndCompleteStreamDoesNotAlloc(t *testing.T) {
	c2s := layers.TCP{
		SrcPort:   1,
		DstPort:   2,
		Seq:       0,
		SYN:       true,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	s2c := layers.TCP{
		SrcPort:   c2s.DstPort,
		DstPort:   c2s.SrcPort,
		Seq:       0,
		SYN:       true,
		ACK:       true,
		BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 0}},
	}
	tf := testMemoryFactory{}
	a := NewAssembler(NewStreamPool(&tf))

	ctx := &testMemoryContext{}
	// First packet
	a.AssembleWithContext(netFlow, &c2s, ctx)
	a.AssembleWithContext(netFlow.Reverse(), &s2c, ctx)
	c2s.SYN, s2c.SYN = false, false
	c2s.ACK = true
	c2s.Seq++
	s2c.Seq++
	N := 1000
	if n := testing.AllocsPerRun(N, func() {
		c2s.Seq += 10
		s2c.Seq += 10
		c2s.Ack += 10
		s2c.Ack += 10
		a.AssembleWithContext(netFlow, &c2s, ctx)
		a.AssembleWithContext(netFlow.Reverse(), &s2c, ctx)
	}); n > 0 {
		t.Error(n, "mallocs for normal TCP stream")
	}
	// Ensure all bytes have been through the stream
	// +1 for first packet and +1 because AllocsPerRun seems to run fun N+1 times.
	if tf.bytes != 10*2*(N+1+1) {
		t.Error(tf.bytes, "bytes handled, expected", 10*2*(N+1+1))
	}
}
