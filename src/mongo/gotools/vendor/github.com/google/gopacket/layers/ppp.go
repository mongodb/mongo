// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"encoding/binary"
	"errors"
	"github.com/google/gopacket"
)

// PPP is the layer for PPP encapsulation headers.
type PPP struct {
	BaseLayer
	PPPType PPPType
}

// PPPEndpoint is a singleton endpoint for PPP.  Since there is no actual
// addressing for the two ends of a PPP connection, we use a singleton value
// named 'point' for each endpoint.
var PPPEndpoint = gopacket.NewEndpoint(EndpointPPP, nil)

// PPPFlow is a singleton flow for PPP.  Since there is no actual addressing for
// the two ends of a PPP connection, we use a singleton value to represent the
// flow for all PPP connections.
var PPPFlow = gopacket.NewFlow(EndpointPPP, nil, nil)

// LayerType returns LayerTypePPP
func (p *PPP) LayerType() gopacket.LayerType { return LayerTypePPP }

// LinkFlow returns PPPFlow.
func (p *PPP) LinkFlow() gopacket.Flow { return PPPFlow }

func decodePPP(data []byte, p gopacket.PacketBuilder) error {
	ppp := &PPP{}
	if data[0]&0x1 == 0 {
		if data[1]&0x1 == 0 {
			return errors.New("PPP has invalid type")
		}
		ppp.PPPType = PPPType(binary.BigEndian.Uint16(data[:2]))
		ppp.Contents = data[:2]
		ppp.Payload = data[2:]
	} else {
		ppp.PPPType = PPPType(data[0])
		ppp.Contents = data[:1]
		ppp.Payload = data[1:]
	}
	p.AddLayer(ppp)
	p.SetLinkLayer(ppp)
	return p.NextDecoder(ppp.PPPType)
}

// SerializeTo writes the serialized form of this layer into the
// SerializationBuffer, implementing gopacket.SerializableLayer.
// See the docs for gopacket.SerializableLayer for more info.
func (p *PPP) SerializeTo(b gopacket.SerializeBuffer, opts gopacket.SerializeOptions) error {
	if p.PPPType&0x100 == 0 {
		bytes, err := b.PrependBytes(2)
		if err != nil {
			return err
		}
		binary.BigEndian.PutUint16(bytes, uint16(p.PPPType))
	} else {
		bytes, err := b.PrependBytes(1)
		if err != nil {
			return err
		}
		bytes[0] = uint8(p.PPPType)
	}
	return nil
}
