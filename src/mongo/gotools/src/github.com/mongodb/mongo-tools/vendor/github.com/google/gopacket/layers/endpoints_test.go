// Copyright 2017, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"net"
	"testing"

	"github.com/google/gopacket"
)

func TestNewIPEndpoint(t *testing.T) {
	cases := []struct {
		ip           net.IP
		endpointType gopacket.EndpointType
	}{
		{net.ParseIP("192.168.0.1").To4(), EndpointIPv4},
		{net.ParseIP("192.168.0.1").To16(), EndpointIPv4},
		{net.ParseIP("2001:0db8:85a3:0000:0000:8a2e:0370:7334"), EndpointIPv6},
	}

	for _, c := range cases {
		endpoint := NewIPEndpoint(c.ip)
		if endpoint == gopacket.InvalidEndpoint {
			t.Errorf("Failed to create an IP endpoint for %s (%d-bytes)",
				c.ip, len(c.ip))
		}
		if endpoint.EndpointType() != c.endpointType {
			t.Errorf("Wrong endpoint type created for %s (%d-bytes): expected %s, got %s",
				c.ip, len(c.ip), c.endpointType, endpoint.EndpointType())
		}
	}
}
