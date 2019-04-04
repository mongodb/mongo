// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build ignore

// This binary pulls the list of known MAC
// prefixes from IEEE and writes them out to a go file which is compiled
// into gopacket.  It should be run as follows:
//
//  go run gen.go | gofmt > valid_mac_prefixes.go
package main

import (
	"bufio"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"regexp"
	"time"
)

const header = `// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package macs

// Created by gen.go, don't edit manually
// Generated at %s
// Fetched from %q

// ValidMACPrefixMap maps a valid MAC address prefix to the name of the
// organization that owns the rights to use it.  We map it to a hidden
// variable so it won't show up in godoc, since it's a very large map.
var ValidMACPrefixMap map[[3]byte]string= validMACPrefixMap
var validMACPrefixMap = map[[3]byte]string{
`

var url = flag.String("url", "http://standards.ieee.org/develop/regauth/oui/oui.txt", "URL to fetch MACs from")

func main() {
	fmt.Fprintf(os.Stderr, "Fetching MACs from %q\n", *url)
	resp, err := http.Get(*url)
	if err != nil {
		panic(err)
	}
	defer resp.Body.Close()
	buffered := bufio.NewReader(resp.Body)
	finder := regexp.MustCompile(`^\s*([0-9A-F]{6})\s+\(base 16\)\s+(.*)`)
	fmt.Fprintln(os.Stderr, "Starting write to standard output")
	fmt.Printf(header, time.Now(), *url)
	for {
		line, err := buffered.ReadString('\n')
		if err == io.EOF {
			break
		} else if err != nil {
			panic(err)
		}
		if matches := finder.FindStringSubmatch(line); matches != nil {
			bytes := make([]byte, 3)
			hex.Decode(bytes, []byte(matches[1]))
			company := matches[2]
			if company == "" {
				company = "PRIVATE"
			}
			fmt.Printf("\t[3]byte{%d, %d, %d}: %q,\n", bytes[0], bytes[1], bytes[2], company)
		}
	}
	fmt.Println("}")
}
