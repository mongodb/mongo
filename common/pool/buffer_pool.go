package pool

import (
	"fmt"
	"sync"
)

// BufferPool is a construct for generating and reusing buffers of a
// given size. Useful for avoiding generating too many temporary
// buffers during runtime, which can anger the garbage collector.
type BufferPool struct {
	size int
	pool *sync.Pool //REQUIRES >= go1.3
}

// NewBufferPool returns an initialized BufferPool for
// buffers of the supplied number of bytes.
func NewBufferPool(size int) *BufferPool {
	if size < 0 {
		panic("cannot create BufferPool of negative size")
	}
	bp := &BufferPool{
		size: size,
		pool: &sync.Pool{
			New: func() interface{} {
				return make([]byte, size)
			},
		},
	}
	return bp
}

// Get returns a new or recycled buffer from the pool.
func (bp *BufferPool) Get() []byte {
	return bp.pool.Get().([]byte)
}

// Put returns the supplied slice back to the buffer.
// Panics if the buffer is of improper size.
func (bp *BufferPool) Put(buffer []byte) {
	if len(buffer) != bp.size {
		panic(fmt.Sprintf(
			"attempting to return a byte buffer of size %v to a BufferPool of size %v",
			len(buffer), bp.size))
	}
	bp.pool.Put(buffer)
}
