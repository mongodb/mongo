// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"github.com/google/gopacket"
)

// EAPOL defines an EAP over LAN (802.1x) layer.
type EAPOL struct {
	BaseLayer
	Version uint8
	Type    EAPOLType
}

// LayerType returns LayerTypeEAPOL.
func (e *EAPOL) LayerType() gopacket.LayerType { return LayerTypeEAPOL }

// DecodeFromBytes decodes the given bytes into this layer.
func (e *EAPOL) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	e.Version = data[0]
	e.Type = EAPOLType(data[1])
	e.BaseLayer = BaseLayer{data[:2], data[2:]}
	return nil
}

// CanDecode returns the set of layer types that this DecodingLayer can decode.
func (e *EAPOL) CanDecode() gopacket.LayerClass {
	return LayerTypeEAPOL
}

// NextLayerType returns the layer type contained by this DecodingLayer.
func (e *EAPOL) NextLayerType() gopacket.LayerType {
	return e.Type.LayerType()
}

func decodeEAPOL(data []byte, p gopacket.PacketBuilder) error {
	e := &EAPOL{}
	return decodingLayerDecoder(e, data, p)
}
