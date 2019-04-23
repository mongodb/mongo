// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build ignore

// This binary handles creating string constants and function templates for enums.
//
//  go run gen.go | gofmt > enums_generated.go
package main

import (
	"fmt"
	"log"
	"os"
	"text/template"
	"time"
)

const fmtString = `// Copyright 2012 Google, Inc. All rights reserved.

package layers

// Created by gen2.go, don't edit manually
// Generated at %s

import (
  "fmt"

  "github.com/google/gopacket"
)

`

var funcsTmpl = template.Must(template.New("foo").Parse(`
// Decoder calls {{.Name}}Metadata.DecodeWith's decoder.
func (a {{.Name}}) Decode(data []byte, p gopacket.PacketBuilder) error {
	return {{.Name}}Metadata[a].DecodeWith.Decode(data, p)
}
// String returns {{.Name}}Metadata.Name.
func (a {{.Name}}) String() string {
	return {{.Name}}Metadata[a].Name
}
// LayerType returns {{.Name}}Metadata.LayerType.
func (a {{.Name}}) LayerType() gopacket.LayerType {
	return {{.Name}}Metadata[a].LayerType
}

type errorDecoderFor{{.Name}} int
func (a *errorDecoderFor{{.Name}}) Decode(data []byte, p gopacket.PacketBuilder) error {
  return a
}
func (a *errorDecoderFor{{.Name}}) Error() string {
  return fmt.Sprintf("Unable to decode {{.Name}} %d", int(*a))
}

var errorDecodersFor{{.Name}} [{{.Num}}]errorDecoderFor{{.Name}}
var {{.Name}}Metadata [{{.Num}}]EnumMetadata

func initUnknownTypesFor{{.Name}}() {
  for i := 0; i < {{.Num}}; i++ {
    errorDecodersFor{{.Name}}[i] = errorDecoderFor{{.Name}}(i)
    {{.Name}}Metadata[i] = EnumMetadata{
      DecodeWith: &errorDecodersFor{{.Name}}[i],
      Name: "Unknown{{.Name}}",
    }
  }
}
`))

func main() {
	fmt.Fprintf(os.Stderr, "Writing results to stdout\n")
	fmt.Printf(fmtString, time.Now())
	types := []struct {
		Name string
		Num  int
	}{
		{"LinkType", 256},
		{"EthernetType", 65536},
		{"PPPType", 65536},
		{"IPProtocol", 256},
		{"SCTPChunkType", 256},
		{"PPPoECode", 256},
		{"FDDIFrameControl", 256},
		{"EAPOLType", 256},
		{"ProtocolFamily", 256},
		{"Dot11Type", 256},
		{"USBTransportType", 256},
	}

	fmt.Println("func init() {")
	for _, t := range types {
		fmt.Printf("initUnknownTypesFor%s()\n", t.Name)
	}
	fmt.Println("initActualTypeData()")
	fmt.Println("}")
	for _, t := range types {
		if err := funcsTmpl.Execute(os.Stdout, t); err != nil {
			log.Fatalf("Failed to execute template %s: %v", t.Name, err)
		}
	}
}
