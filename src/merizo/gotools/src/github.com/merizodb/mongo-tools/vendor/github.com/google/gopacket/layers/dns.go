// Copyright 2014 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"encoding/binary"
	"errors"
	"fmt"
	"github.com/google/gopacket"
	"net"
)

type DNSClass uint16

const (
	DNSClassIN  DNSClass = 1   // Internet
	DNSClassCS  DNSClass = 2   // the CSNET class (Obsolete)
	DNSClassCH  DNSClass = 3   // the CHAOS class
	DNSClassHS  DNSClass = 4   // Hesiod [Dyer 87]
	DNSClassAny DNSClass = 255 // AnyClass
)

type DNSType uint16

const (
	DNSTypeA     DNSType = 1  // a host address
	DNSTypeNS    DNSType = 2  // an authoritative name server
	DNSTypeMD    DNSType = 3  // a mail destination (Obsolete - use MX)
	DNSTypeMF    DNSType = 4  // a mail forwarder (Obsolete - use MX)
	DNSTypeCNAME DNSType = 5  // the canonical name for an alias
	DNSTypeSOA   DNSType = 6  // marks the start of a zone of authority
	DNSTypeMB    DNSType = 7  // a mailbox domain name (EXPERIMENTAL)
	DNSTypeMG    DNSType = 8  // a mail group member (EXPERIMENTAL)
	DNSTypeMR    DNSType = 9  // a mail rename domain name (EXPERIMENTAL)
	DNSTypeNULL  DNSType = 10 // a null RR (EXPERIMENTAL)
	DNSTypeWKS   DNSType = 11 // a well known service description
	DNSTypePTR   DNSType = 12 // a domain name pointer
	DNSTypeHINFO DNSType = 13 // host information
	DNSTypeMINFO DNSType = 14 // mailbox or mail list information
	DNSTypeMX    DNSType = 15 // mail exchange
	DNSTypeTXT   DNSType = 16 // text strings
	DNSTypeAAAA  DNSType = 28 // a IPv6 host address [RFC3596]
	DNSTypeSRV   DNSType = 33 // server discovery [RFC2782] [RFC6195]
)

type DNSResponseCode uint8

const (
	DNSResponseCodeFormErr  DNSResponseCode = 1  // Format Error                       [RFC1035]
	DNSResponseCodeServFail DNSResponseCode = 2  // Server Failure                     [RFC1035]
	DNSResponseCodeNXDomain DNSResponseCode = 3  // Non-Existent Domain                [RFC1035]
	DNSResponseCodeNotImp   DNSResponseCode = 4  // Not Implemented                    [RFC1035]
	DNSResponseCodeRefused  DNSResponseCode = 5  // Query Refused                      [RFC1035]
	DNSResponseCodeYXDomain DNSResponseCode = 6  // Name Exists when it should not     [RFC2136]
	DNSResponseCodeYXRRSet  DNSResponseCode = 7  // RR Set Exists when it should not   [RFC2136]
	DNSResponseCodeNXRRSet  DNSResponseCode = 8  // RR Set that should exist does not  [RFC2136]
	DNSResponseCodeNotAuth  DNSResponseCode = 9  // Server Not Authoritative for zone  [RFC2136]
	DNSResponseCodeNotZone  DNSResponseCode = 10 // Name not contained in zone         [RFC2136]
	DNSResponseCodeBadVers  DNSResponseCode = 16 // Bad OPT Version                    [RFC2671]
	DNSResponseCodeBadSig   DNSResponseCode = 16 // TSIG Signature Failure             [RFC2845]
	DNSResponseCodeBadKey   DNSResponseCode = 17 // Key not recognized                 [RFC2845]
	DNSResponseCodeBadTime  DNSResponseCode = 18 // Signature out of time window       [RFC2845]
	DNSResponseCodeBadMode  DNSResponseCode = 19 // Bad TKEY Mode                      [RFC2930]
	DNSResponseCodeBadName  DNSResponseCode = 20 // Duplicate key name                 [RFC2930]
	DNSResponseCodeBadAlg   DNSResponseCode = 21 // Algorithm not supported            [RFC2930]
	DNSResponseCodeBadTruc  DNSResponseCode = 22 // Bad Truncation                     [RFC4635]
)

func (drc DNSResponseCode) String() string {
	switch drc {
	default:
		return "Unknown"
	case DNSResponseCodeFormErr:
		return "Format Error"
	case DNSResponseCodeServFail:
		return "Server Failure "
	case DNSResponseCodeNXDomain:
		return "Non-Existent Domain"
	case DNSResponseCodeNotImp:
		return "Not Implemented"
	case DNSResponseCodeRefused:
		return "Query Refused"
	case DNSResponseCodeYXDomain:
		return "Name Exists when it should not"
	case DNSResponseCodeYXRRSet:
		return "RR Set Exists when it should not"
	case DNSResponseCodeNXRRSet:
		return "RR Set that should exist does not"
	case DNSResponseCodeNotAuth:
		return "Server Not Authoritative for zone"
	case DNSResponseCodeNotZone:
		return "Name not contained in zone"
	case DNSResponseCodeBadVers:
		return "Bad OPT Version"
	case DNSResponseCodeBadKey:
		return "Key not recognized"
	case DNSResponseCodeBadTime:
		return "Signature out of time window"
	case DNSResponseCodeBadMode:
		return "Bad TKEY Mode"
	case DNSResponseCodeBadName:
		return "Duplicate key name"
	case DNSResponseCodeBadAlg:
		return "Algorithm not supported"
	case DNSResponseCodeBadTruc:
		return "Bad Truncation"
	}
}

type DNSOpCode uint8

const (
	DNSOpCodeQuery  DNSOpCode = 0 // Query                  [RFC1035]
	DNSOpCodeIQuery DNSOpCode = 1 // Inverse Query Obsolete [RFC3425]
	DNSOpCodeStatus DNSOpCode = 2 // Status                 [RFC1035]
	DNSOpCodeNotify DNSOpCode = 4 // Notify                 [RFC1996]
	DNSOpCodeUpdate DNSOpCode = 5 // Update                 [RFC2136]
)

// DNS is specified in RFC 1034 / RFC 1035
// +---------------------+
// |        Header       |
// +---------------------+
// |       Question      | the question for the name server
// +---------------------+
// |        Answer       | RRs answering the question
// +---------------------+
// |      Authority      | RRs pointing toward an authority
// +---------------------+
// |      Additional     | RRs holding additional information
// +---------------------+
//
//  DNS Header
//  0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                      ID                       |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |QR|   Opcode  |AA|TC|RD|RA|   Z    |   RCODE   |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    QDCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    ANCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    NSCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    ARCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+

// DNS contains data from a single Domain Name Service packet.
type DNS struct {
	BaseLayer

	// Header fields
	ID     uint16
	QR     bool
	OpCode DNSOpCode

	AA bool  // Authoritative answer
	TC bool  // Truncated
	RD bool  // Recursion desired
	RA bool  // Recursion available
	Z  uint8 // Resrved for future use

	ResponseCode DNSResponseCode
	QDCount      uint16 // Number of questions to expect
	ANCount      uint16 // Number of answers to expect
	NSCount      uint16 // Number of authorities to expect
	ARCount      uint16 // Number of additional records to expect

	// Entries
	Questions   []DNSQuestion
	Answers     []DNSResourceRecord
	Authorities []DNSResourceRecord
	Additionals []DNSResourceRecord

	// buffer for doing name decoding.  We use a single reusable buffer to avoid
	// name decoding on a single object via multiple DecodeFromBytes calls
	// requiring constant allocation of small byte slices.
	buffer []byte
}

// LayerType returns gopacket.LayerTypeDNS.
func (d *DNS) LayerType() gopacket.LayerType { return LayerTypeDNS }

// decodeDNS decodes the byte slice into a DNS type. It also
// setups the application Layer in PacketBuilder.
func decodeDNS(data []byte, p gopacket.PacketBuilder) error {
	d := &DNS{}
	err := d.DecodeFromBytes(data, p)
	if err != nil {
		return err
	}
	p.AddLayer(d)
	p.SetApplicationLayer(d)
	return nil
}

// DecodeFromBytes decodes the slice into the DNS struct.
func (d *DNS) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	d.buffer = d.buffer[:0]

	if len(data) < 12 {
		df.SetTruncated()
		return fmt.Errorf("DNS packet too short")
	}

	// since there are no further layers, the baselayer's content is
	// pointing to this layer
	d.BaseLayer = BaseLayer{Contents: data[:len(data)]}
	d.ID = binary.BigEndian.Uint16(data[:2])
	d.QR = data[2]&0x80 != 0
	d.OpCode = DNSOpCode(data[2]>>3) & 0x0F
	d.AA = data[2]&0x04 != 0
	d.TC = data[2]&0x02 != 0
	d.RD = data[2]&0x01 != 0
	d.RA = data[3]&0x80 != 0
	d.Z = uint8(data[3]>>4) & 0x7
	d.ResponseCode = DNSResponseCode(data[3] & 0xF)
	d.QDCount = binary.BigEndian.Uint16(data[4:6])
	d.ANCount = binary.BigEndian.Uint16(data[6:8])
	d.NSCount = binary.BigEndian.Uint16(data[8:10])
	d.ARCount = binary.BigEndian.Uint16(data[10:12])

	d.Questions = d.Questions[:0]
	d.Answers = d.Answers[:0]
	d.Authorities = d.Authorities[:0]
	d.Additionals = d.Additionals[:0]

	offset := 12
	var err error
	for i := 0; i < int(d.QDCount); i++ {
		var q DNSQuestion
		if offset, err = q.decode(data, offset, df, &d.buffer); err != nil {
			return err
		}
		d.Questions = append(d.Questions, q)
	}

	// For some horrible reason, if we do the obvious thing in this loop:
	//   var r DNSResourceRecord
	//   if blah := r.decode(blah); err != nil {
	//     return err
	//   }
	//   d.Foo = append(d.Foo, r)
	// the Go compiler thinks that 'r' escapes to the heap, causing a malloc for
	// every Answer, Authority, and Additional.  To get around this, we do
	// something really silly:  we append an empty resource record to our slice,
	// then use the last value in the slice to call decode.  Since the value is
	// already in the slice, there's no WAY it can escape... on the other hand our
	// code is MUCH uglier :(
	for i := 0; i < int(d.ANCount); i++ {
		d.Answers = append(d.Answers, DNSResourceRecord{})
		if offset, err = d.Answers[i].decode(data, offset, df, &d.buffer); err != nil {
			d.Answers = d.Answers[:i] // strip off erroneous value
			return err
		}
	}
	for i := 0; i < int(d.NSCount); i++ {
		d.Authorities = append(d.Authorities, DNSResourceRecord{})
		if offset, err = d.Authorities[i].decode(data, offset, df, &d.buffer); err != nil {
			d.Authorities = d.Authorities[:i] // strip off erroneous value
			return err
		}
	}
	for i := 0; i < int(d.ARCount); i++ {
		d.Additionals = append(d.Additionals, DNSResourceRecord{})
		if offset, err = d.Additionals[i].decode(data, offset, df, &d.buffer); err != nil {
			d.Additionals = d.Additionals[:i] // strip off erroneous value
			return err
		}
	}

	if uint16(len(d.Questions)) != d.QDCount {
		return errors.New("Invalid query decoding, not the right number of questions")
	} else if uint16(len(d.Answers)) != d.ANCount {
		return errors.New("Invalid query decoding, not the right number of answers")
	} else if uint16(len(d.Authorities)) != d.NSCount {
		return errors.New("Invalid query decoding, not the right number of authorities")
	} else if uint16(len(d.Additionals)) != d.ARCount {
		return errors.New("Invalid query decoding, not the right number of additionals info")
	}
	return nil
}

func (d *DNS) CanDecode() gopacket.LayerClass {
	return LayerTypeDNS
}

func (d *DNS) NextLayerType() gopacket.LayerType {
	return gopacket.LayerTypePayload
}

func (d *DNS) Payload() []byte {
	return nil
}

var maxRecursion = errors.New("max DNS recursion level hit")

const maxRecursionLevel = 255

func decodeName(data []byte, offset int, buffer *[]byte, level int) ([]byte, int, error) {
	if level > maxRecursionLevel {
		return nil, 0, maxRecursion
	}
	start := len(*buffer)
	index := offset
	if data[index] == 0x00 {
		return nil, index + 1, nil
	}
loop:
	for data[index] != 0x00 {
		switch data[index] & 0xc0 {
		default:
			/* RFC 1035
			   A domain name represented as a sequence of labels, where
			   each label consists of a length octet followed by that
			   number of octets.  The domain name terminates with the
			   zero length octet for the null label of the root.  Note
			   that this field may be an odd number of octets; no
			   padding is used.
			*/
			index2 := index + int(data[index]) + 1
			if index2-offset > 255 {
				return nil, 0,
					fmt.Errorf("dns name is too long")
			}
			*buffer = append(*buffer, '.')
			*buffer = append(*buffer, data[index+1:index2]...)
			index = index2

		case 0xc0:
			/* RFC 1035
			   The pointer takes the form of a two octet sequence.

			   The first two bits are ones.  This allows a pointer to
			   be distinguished from a label, since the label must
			   begin with two zero bits because labels are restricted
			   to 63 octets or less.  (The 10 and 01 combinations are
			   reserved for future use.)  The OFFSET field specifies
			   an offset from the start of the message (i.e., the
			   first octet of the ID field in the domain header).  A
			   zero offset specifies the first byte of the ID field,
			   etc.

			   The compression scheme allows a domain name in a message to be
			   represented as either:
			      - a sequence of labels ending in a zero octet
			      - a pointer
			      - a sequence of labels ending with a pointer
			*/

			offsetp := int(binary.BigEndian.Uint16(data[index:index+2]) & 0x3fff)
			// This looks a little tricky, but actually isn't.  Because of how
			// decodeName is written, calling it appends the decoded name to the
			// current buffer.  We already have the start of the buffer, then, so
			// once this call is done buffer[start:] will contain our full name.
			_, _, err := decodeName(data, offsetp, buffer, level+1)
			if err != nil {
				return nil, 0, err
			}
			index++ // pointer is two bytes, so add an extra byte here.
			break loop
		/* EDNS, or other DNS option ? */
		case 0x40: // RFC 2673
			return nil, 0, fmt.Errorf("qname '0x40' - RFC 2673 unsupported yet (data=%x index=%d)",
				data[index], index)

		case 0x80:
			return nil, 0, fmt.Errorf("qname '0x80' unsupported yet (data=%x index=%d)",
				data[index], index)
		}
	}
	return (*buffer)[start+1:], index + 1, nil
}

type DNSQuestion struct {
	Name  []byte
	Type  DNSType
	Class DNSClass
}

func (q *DNSQuestion) decode(data []byte, offset int, df gopacket.DecodeFeedback, buffer *[]byte) (int, error) {
	name, endq, err := decodeName(data, offset, buffer, 1)
	if err != nil {
		return 0, err
	}

	q.Name = name
	q.Type = DNSType(binary.BigEndian.Uint16(data[endq : endq+2]))
	q.Class = DNSClass(binary.BigEndian.Uint16(data[endq+2 : endq+4]))

	return endq + 4, nil
}

//  DNSResourceRecord
//  0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                                               |
//  /                                               /
//  /                      NAME                     /
//  |                                               |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                      TYPE                     |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                     CLASS                     |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                      TTL                      |
//  |                                               |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                   RDLENGTH                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--|
//  /                     RDATA                     /
//  /                                               /
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+

type DNSResourceRecord struct {
	// Header
	Name  []byte
	Type  DNSType
	Class DNSClass
	TTL   uint32

	// RDATA Raw Values
	DataLength uint16
	Data       []byte

	// RDATA Decoded Values
	IP             net.IP
	NS, CNAME, PTR []byte
	TXTs           [][]byte
	SOA            DNSSOA
	SRV            DNSSRV
	MX             DNSMX

	// Undecoded TXT for backward compatibility
	TXT []byte
}

// decode decodes the resource record, returning the total length of the record.
func (rr *DNSResourceRecord) decode(data []byte, offset int, df gopacket.DecodeFeedback, buffer *[]byte) (int, error) {
	name, endq, err := decodeName(data, offset, buffer, 1)
	if err != nil {
		return 0, err
	}

	rr.Name = name
	rr.Type = DNSType(binary.BigEndian.Uint16(data[endq : endq+2]))
	rr.Class = DNSClass(binary.BigEndian.Uint16(data[endq+2 : endq+4]))
	rr.TTL = binary.BigEndian.Uint32(data[endq+4 : endq+8])
	rr.DataLength = binary.BigEndian.Uint16(data[endq+8 : endq+10])
	rr.Data = data[endq+10 : endq+10+int(rr.DataLength)]

	if err = rr.decodeRData(data, endq+10, buffer); err != nil {
		return 0, err
	}

	return endq + 10 + int(rr.DataLength), nil
}

func (rr *DNSResourceRecord) String() string {
	if (rr.Class == DNSClassIN) && ((rr.Type == DNSTypeA) || (rr.Type == DNSTypeAAAA)) {
		return net.IP(rr.Data).String()
	}
	return "..."
}

func decodeCharacterStrings(data []byte) ([][]byte, error) {
	strings := make([][]byte, 0, 1)
	end := len(data)
	for index, index2 := 0, 0; index != end; index = index2 {
		index2 = index + 1 + int(data[index]) // index increases by 1..256 and does not overflow
		if index2 > end {
			return nil, errors.New("Insufficient data for a <character-string>")
		}
		strings = append(strings, data[index+1:index2])
	}
	return strings, nil
}

func (rr *DNSResourceRecord) decodeRData(data []byte, offset int, buffer *[]byte) error {
	switch rr.Type {
	case DNSTypeA:
		rr.IP = rr.Data
	case DNSTypeAAAA:
		rr.IP = rr.Data
	case DNSTypeTXT, DNSTypeHINFO:
		rr.TXT = rr.Data
		txts, err := decodeCharacterStrings(rr.Data)
		if err != nil {
			return err
		}
		rr.TXTs = txts
	case DNSTypeNS:
		name, _, err := decodeName(data, offset, buffer, 1)
		if err != nil {
			return err
		}
		rr.NS = name
	case DNSTypeCNAME:
		name, _, err := decodeName(data, offset, buffer, 1)
		if err != nil {
			return err
		}
		rr.CNAME = name
	case DNSTypePTR:
		name, _, err := decodeName(data, offset, buffer, 1)
		if err != nil {
			return err
		}
		rr.PTR = name
	case DNSTypeSOA:
		name, endq, err := decodeName(data, offset, buffer, 1)
		if err != nil {
			return err
		}
		rr.SOA.MName = name
		name, endq, err = decodeName(data, endq, buffer, 1)
		if err != nil {
			return err
		}
		rr.SOA.RName = name
		rr.SOA.Serial = binary.BigEndian.Uint32(data[endq : endq+4])
		rr.SOA.Refresh = binary.BigEndian.Uint32(data[endq+4 : endq+8])
		rr.SOA.Retry = binary.BigEndian.Uint32(data[endq+8 : endq+12])
		rr.SOA.Expire = binary.BigEndian.Uint32(data[endq+12 : endq+16])
		rr.SOA.Minimum = binary.BigEndian.Uint32(data[endq+16 : endq+20])
	case DNSTypeMX:
		rr.MX.Preference = binary.BigEndian.Uint16(data[offset : offset+2])
		name, _, err := decodeName(data, offset+2, buffer, 1)
		if err != nil {
			return err
		}
		rr.MX.Name = name
	case DNSTypeSRV:
		rr.SRV.Priority = binary.BigEndian.Uint16(data[offset : offset+2])
		rr.SRV.Weight = binary.BigEndian.Uint16(data[offset+2 : offset+4])
		rr.SRV.Port = binary.BigEndian.Uint16(data[offset+4 : offset+6])
		name, _, err := decodeName(data, offset+6, buffer, 1)
		if err != nil {
			return err
		}
		rr.SRV.Name = name
	}
	return nil
}

type DNSSOA struct {
	MName, RName                            []byte
	Serial, Refresh, Retry, Expire, Minimum uint32
}

type DNSSRV struct {
	Priority, Weight, Port uint16
	Name                   []byte
}

type DNSMX struct {
	Preference uint16
	Name       []byte
}
