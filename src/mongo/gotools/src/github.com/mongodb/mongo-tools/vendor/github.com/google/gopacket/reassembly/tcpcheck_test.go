// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package reassembly

import (
	"testing"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

// netFlow declared in tcpassembly_test

/*
 * FSM tests
 */

type testCheckFSMSequence struct {
	tcp      layers.TCP
	ci       gopacket.CaptureInfo
	expected bool
}

func testCheckFSM(t *testing.T, options TCPSimpleFSMOptions, s []testCheckFSMSequence) {
	fsm := NewTCPSimpleFSM(options)
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
		res := fsm.CheckState(&test.tcp, dir)
		if res != test.expected {
			t.Fatalf("#%d: packet rejected (%v): got %v, expected %v. State:%s", i, gopacket.LayerDump(&test.tcp), res, test.expected, fsm.String())
		}
	}
}

func TestCheckFSM(t *testing.T) {
	testCheckFSM(t, TCPSimpleFSMOptions{}, []testCheckFSMSequence{
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
			expected: true,
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
			expected: true,
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
			expected: true,
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
			expected: true,
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
			expected: true,
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
			expected: true,
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
			expected: true,
		},
	})
}

func TestCheckFSMmissingSYNACK(t *testing.T) {
	testCheckFSM(t, TCPSimpleFSMOptions{}, []testCheckFSMSequence{
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
			expected: true,
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
			expected: false,
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
			expected: false,
		},
	})
}

// Support packets w/o SYN (+SYN+ACK) depending on option
func TestCheckFSMmissingSYN(t *testing.T) {
	for _, val := range []bool{false, true} {
		testCheckFSM(t, TCPSimpleFSMOptions{SupportMissingEstablishment: val}, []testCheckFSMSequence{
			{
				tcp: layers.TCP{
					ACK:       true,
					SrcPort:   54842,
					DstPort:   53,
					Seq:       12,
					Ack:       1012,
					BaseLayer: layers.BaseLayer{Payload: []byte{1}},
				},
				ci: gopacket.CaptureInfo{
					Timestamp: time.Unix(1432538521, 566690000),
				},
				expected: val,
			},
			{
				tcp: layers.TCP{
					ACK:       true,
					SrcPort:   53,
					DstPort:   54842,
					Seq:       1012,
					Ack:       13,
					BaseLayer: layers.BaseLayer{Payload: []byte{2}},
				},
				ci: gopacket.CaptureInfo{
					Timestamp: time.Unix(1432538521, 590346000),
				},
				expected: val,
			},
			{
				tcp: layers.TCP{
					ACK:       true,
					SrcPort:   53,
					DstPort:   54842,
					Seq:       1013,
					Ack:       13,
					BaseLayer: layers.BaseLayer{Payload: []byte{3}},
				},
				ci: gopacket.CaptureInfo{
					Timestamp: time.Unix(1432538521, 590387000),
				},
				expected: val,
			},
		})
	}
}

/*
 * Option tests
 */

type testCheckOptionsSequence struct {
	tcp      layers.TCP
	ci       gopacket.CaptureInfo
	dir      TCPFlowDirection
	nextSeq  Sequence
	expected bool
	start    bool
}

func testCheckOptions(t *testing.T, title string, s []testCheckOptionsSequence) {
	opt := NewTCPOptionCheck()
	for i, test := range s {
		err := opt.Accept(&test.tcp, test.ci, test.dir, test.nextSeq, &test.start)
		res := err == nil
		if res != test.expected {
			t.Fatalf("'%v' #%d: packet rejected (%v): got %v, expected %v.", title, i, gopacket.LayerDump(&test.tcp), res, test.expected)
		}
	}
}

func TestCheckOptions(t *testing.T) {
	for _, test := range []struct {
		title    string
		sequence []testCheckOptionsSequence
	}{
		{
			title: "simle valid flow",
			sequence: []testCheckOptionsSequence{
				{
					dir:     TCPDirClientToServer,
					nextSeq: -1, // no packets received yet.
					tcp: layers.TCP{
						SrcPort:   35721,
						DstPort:   80,
						Seq:       374511116,
						Ack:       0,
						BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 566690000),
					},
					expected: true,
				},
				{
					dir:     TCPDirServerToClient,
					nextSeq: -1,
					tcp: layers.TCP{
						SrcPort:   53,
						DstPort:   54842,
						Seq:       3465787765,
						Ack:       374511119,
						BaseLayer: layers.BaseLayer{Payload: []byte{}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 590332000),
					},
					expected: true,
				},
				{
					dir:     TCPDirClientToServer,
					nextSeq: 374511119,
					tcp: layers.TCP{
						ACK:       true,
						SrcPort:   54842,
						DstPort:   53,
						Seq:       374511119,
						Ack:       3465787766,
						BaseLayer: layers.BaseLayer{Payload: []byte{2, 3, 4}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 590346000),
					},
					expected: true,
				},
			},
		},
		{
			title: "ack received before data",
			sequence: []testCheckOptionsSequence{
				{
					dir:     TCPDirServerToClient,
					nextSeq: -1,
					tcp: layers.TCP{
						SrcPort:   53,
						DstPort:   54842,
						Seq:       3465787765,
						Ack:       374511119,
						BaseLayer: layers.BaseLayer{Payload: []byte{}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 590332000),
					},
					expected: true,
				},
				{
					dir:     TCPDirClientToServer,
					nextSeq: 37451116, // this is the next expected sequence.
					tcp: layers.TCP{
						SrcPort:   35721,
						DstPort:   80,
						Seq:       374511116,
						Ack:       0,
						BaseLayer: layers.BaseLayer{Payload: []byte{1, 2, 3}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 566690000),
					},
					expected: true,
				},
				{
					dir:     TCPDirClientToServer,
					nextSeq: 374511119,
					tcp: layers.TCP{
						ACK:       true,
						SrcPort:   54842,
						DstPort:   53,
						Seq:       374511119,
						Ack:       3465787766,
						BaseLayer: layers.BaseLayer{Payload: []byte{2, 3, 4}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 590346000),
					},
					expected: true,
				},
				{
					dir:     TCPDirClientToServer,
					nextSeq: 374511122, // 10 bytes skipped
					tcp: layers.TCP{
						ACK:       true,
						SrcPort:   54842,
						DstPort:   53,
						Seq:       374511132,
						Ack:       3465787766,
						BaseLayer: layers.BaseLayer{Payload: []byte{22, 33, 44}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 590346000),
					},
					expected: true,
				},
				{
					dir:     TCPDirClientToServer,
					nextSeq: 374511132,
					tcp: layers.TCP{
						ACK:       true,
						SrcPort:   54842,
						DstPort:   53,
						Seq:       374511119, // retransmission of reassembled data.
						Ack:       3465787766,
						BaseLayer: layers.BaseLayer{Payload: []byte{2, 3, 4}},
					},
					ci: gopacket.CaptureInfo{
						Timestamp: time.Unix(1432538521, 590346000),
					},
					expected: false,
				},
			},
		},
	} {
		testCheckOptions(t, test.title, test.sequence)
	}
}
