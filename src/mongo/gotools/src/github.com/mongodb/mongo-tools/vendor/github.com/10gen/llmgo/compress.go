package mgo

import (
	"fmt"
	"io"

	"github.com/golang/snappy"
)

type messageCompressor interface {
	getId() uint8
	getName() string

	getMaxCompressedSize(int) int
	compressData(dst, src []byte) (int, error)
	decompressData(dst, src []byte) (int, error)
}

const (
	noopCompressorId   = 0
	snappyCompressorId = 1
)

var (
	preferredMessageCompressor = new(snappyMessageCompressor)
	MessageCompressorRegistry  = messageCompressorRegistry{
		tbl: map[uint8]messageCompressor{
			noopCompressorId:   new(noopMessageCompressor),
			snappyCompressorId: new(snappyMessageCompressor),
		},
	}
)

type messageCompressorRegistry struct {
	tbl map[uint8]messageCompressor
}

func (r messageCompressorRegistry) register(impl messageCompressor) {
	r.tbl[impl.getId()] = impl
}

func (r messageCompressorRegistry) get(id uint8) messageCompressor {
	return r.tbl[id]
}

func (r messageCompressorRegistry) getIds() (ids []uint8) {
	for k := range r.tbl {
		ids = append(ids, k)
	}
	return
}

type compressionHeader struct {
	originalOpCode   int32
	uncompressedSize int32
	compressedId     uint8
}

func (c *compressionHeader) serialize(p []byte) {
	setInt32(p, 0, c.originalOpCode)
	setInt32(p, 4, c.uncompressedSize)
	p[8] = byte(c.compressedId)
}

func (c *compressionHeader) extract(p []byte) {
	c.originalOpCode = getInt32(p, 0)
	c.uncompressedSize = getInt32(p, 4)
	c.compressedId = uint8(p[8])
}

// CompressMessage compresses a message p (header included) using the
// preferred message compressor.
func CompressMessage(p []byte) (o []byte, err error) {
	// TODO select compression based on handshake
	mc := preferredMessageCompressor
	if mc == nil {
		o = p
		return
	}
	logf("compressing message with %s", mc.getName())
	bufferSize := int32(mc.getMaxCompressedSize(len(p)-16) +
		16 + 9) // msg header + compression header
	c := &compressionHeader{
		originalOpCode:   getInt32(p, 0),
		uncompressedSize: int32(len(p) - 16),
		compressedId:     mc.getId(),
	}
	o = make([]byte, bufferSize)
	setInt32(o, 0, bufferSize)
	setInt32(o, 4, getInt32(p, 4)) // RequestID
	setInt32(o, 8, getInt32(p, 8)) // ResponseTo
	setInt32(o, 12, int32(dbCompressed))
	c.serialize(o[16 : 16+9])
	n, err := mc.compressData(o[16+9:], p[16:])
	if err != nil {
		return
	}
	o = o[:16+9+n]
	setInt32(o, 0, int32(16+9+n))
	return
}

// DecompressMessage decompresses an entire message p (header included)
func DecompressMessage(p []byte) (o []byte, err error) {
	c := new(compressionHeader)
	c.extract(p[16 : 16+9])
	mc := MessageCompressorRegistry.get(c.compressedId)
	if mc == nil {
		err = fmt.Errorf("no compression algorithm registered for id '%d'", c.compressedId)
		return
	}
	bufferSize := int32(c.uncompressedSize + 16) // 16 from message header
	o = make([]byte, bufferSize)
	setInt32(o, 0, bufferSize)
	setInt32(o, 4, getInt32(p, 4)) // RequestID
	setInt32(o, 8, getInt32(p, 8)) // ResponseTo
	setInt32(o, 12, c.originalOpCode)
	n, err := mc.decompressData(o[16:], p[16+9:])
	if err != nil {
		return
	}
	o = o[:16+n] // trim extra bytes
	setInt32(o, 0, int32(16+n))
	return
}

/* messageCompressor implementations */

type noopMessageCompressor struct{}

func (noopMessageCompressor) getId() uint8    { return noopCompressorId }
func (noopMessageCompressor) getName() string { return "noop" }
func (noopMessageCompressor) getMaxCompressedSize(srcLen int) int {
	return srcLen
}
func (noopMessageCompressor) compressData(dst, src []byte) (n int, err error) {
	n = copy(dst, src)
	return
}
func (noopMessageCompressor) decompressData(dst, src []byte) (n int, err error) {
	n = copy(dst, src)
	return
}

type snappyMessageCompressor struct{}

func (snappyMessageCompressor) getId() uint8    { return snappyCompressorId }
func (snappyMessageCompressor) getName() string { return "snappy" }
func (snappyMessageCompressor) getMaxCompressedSize(srcLen int) int {
	return snappy.MaxEncodedLen(srcLen)
}
func (snappyMessageCompressor) compressData(dst, src []byte) (n int, err error) {
	p := snappy.Encode(dst, src)
	if len(p) > len(dst) {
		err = io.ErrShortBuffer
	}
	n = len(p)
	return
}
func (snappyMessageCompressor) decompressData(dst, src []byte) (n int, err error) {
	n, err = snappy.DecodedLen(src)
	if err != nil {
		return
	}
	if n < 0 || n > len(dst) {
		err = io.ErrShortBuffer
		return
	}
	_, err = snappy.Decode(dst, src)
	return
}
