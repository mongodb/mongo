// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package topology

import (
	"context"
	"sync"
	"sync/atomic"
	"time"

	"go.mongodb.org/mongo-driver/x/mongo/driver/address"
)

// ErrPoolConnected is returned from an attempt to connect an already connected pool
var ErrPoolConnected = PoolError("pool is connected")

// ErrPoolDisconnected is returned from an attempt to disconnect an already disconnected
// or disconnecting pool.
var ErrPoolDisconnected = PoolError("pool is disconnected or disconnecting")

// ErrConnectionClosed is returned from an attempt to use an already closed connection.
var ErrConnectionClosed = ConnectionError{ConnectionID: "<closed>", message: "connection is closed"}

// ErrWrongPool is return when a connection is returned to a pool it doesn't belong to.
var ErrWrongPool = PoolError("connection does not belong to this pool")

// PoolError is an error returned from a Pool method.
type PoolError string

// pruneInterval is the interval at which the background routine to close expired connections will be run.
var pruneInterval = time.Minute

func (pe PoolError) Error() string { return string(pe) }

type pool struct {
	address    address.Address
	opts       []ConnectionOption
	conns      *resourcePool // pool for idle connections
	generation uint64

	connected int32 // Must be accessed using the sync/atomic package.
	nextid    uint64
	opened    map[uint64]*connection // opened holds all of the currently open connections.

	sync.Mutex
}

func connectionExpiredFunc(v interface{}) bool {
	return v.(*connection).expired()
}

func connectionCloseFunc(v interface{}) {
	c := v.(*connection)
	go c.pool.close(c)
}

// newPool creates a new pool that will hold size number of idle connections. It will use the
// provided options when creating connections.
func newPool(addr address.Address, size uint64, opts ...ConnectionOption) *pool {
	return &pool{
		address:    addr,
		conns:      newResourcePool(size, connectionExpiredFunc, connectionCloseFunc, pruneInterval),
		generation: 0,
		connected:  disconnected,
		opened:     make(map[uint64]*connection),
		opts:       opts,
	}
}

// drain lazily drains the pool by increasing the generation ID.
func (p *pool) drain()                         { atomic.AddUint64(&p.generation, 1) }
func (p *pool) expired(generation uint64) bool { return generation < atomic.LoadUint64(&p.generation) }

// connect puts the pool into the connected state, allowing it to be used.
func (p *pool) connect() error {
	if !atomic.CompareAndSwapInt32(&p.connected, disconnected, connected) {
		return ErrPoolConnected
	}
	atomic.AddUint64(&p.generation, 1)
	return nil
}

func (p *pool) disconnect(ctx context.Context) error {
	if !atomic.CompareAndSwapInt32(&p.connected, connected, disconnecting) {
		return ErrPoolDisconnected
	}

	// We first clear out the idle connections, then we wait until the context's deadline is hit or
	// it's cancelled, after which we aggressively close the remaining open connections.
	for {
		connVal := p.conns.Get()
		if connVal == nil {
			break
		}

		_ = p.close(connVal.(*connection))
	}
	if dl, ok := ctx.Deadline(); ok {
		// If we have a deadline then we interpret it as a request to gracefully shutdown. We wait
		// until either all the connections have landed back in the pool (and have been closed) or
		// until the timer is done.
		ticker := time.NewTicker(1 * time.Second)
		defer ticker.Stop()
		timer := time.NewTimer(time.Now().Sub(dl))
		defer timer.Stop()
		for {
			select {
			case <-timer.C:
			case <-ticker.C: // Can we repalce this with an actual signal channel? We will know when p.inflight hits zero from the close method.
				p.Lock()
				if len(p.opened) > 0 {
					p.Unlock()
					continue
				}
				p.Unlock()
			}
			break
		}
	}

	// We copy the remaining connections into a slice, then iterate it to close them. This allows us
	// to use a single function to actually clean up and close connections at the expense of a
	// double itertion in the worse case.
	p.Lock()
	toClose := make([]*connection, 0, len(p.opened))
	for _, pc := range p.opened {
		toClose = append(toClose, pc)
	}
	p.Unlock()
	for _, pc := range toClose {
		_ = p.close(pc) // We don't care about errors while closing the connection.
	}
	atomic.StoreInt32(&p.connected, disconnected)
	return nil
}

func (p *pool) get(ctx context.Context) (*connection, error) {
	if atomic.LoadInt32(&p.connected) != connected {
		return nil, ErrPoolDisconnected
	}

	// try to get an unexpired idle connection
	connVal := p.conns.Get()
	if connVal != nil {
		return connVal.(*connection), nil
	}

	// couldn't find an unexpired connection. create a new one.
	select {
	case <-ctx.Done():
		return nil, ctx.Err()
	default:
		c, err := newConnection(ctx, p.address, p.opts...)
		if err != nil {
			return nil, err
		}

		c.pool = p
		c.poolID = atomic.AddUint64(&p.nextid, 1)
		c.generation = p.generation

		if atomic.LoadInt32(&p.connected) != connected {
			_ = p.close(c) // The pool is disconnected or disconnecting, ignore the error from closing the connection.
			return nil, ErrPoolDisconnected
		}
		p.Lock()
		p.opened[c.poolID] = c
		p.Unlock()
		return c, nil
	}
}

// close closes a connection, not the pool itself. This method will actually close the connection,
// making it unusable, to instead return the connection to the pool, use put.
func (p *pool) close(c *connection) error {
	if c.pool != p {
		return ErrWrongPool
	}
	p.Lock()
	delete(p.opened, c.poolID)
	p.Unlock()

	if !atomic.CompareAndSwapInt32(&c.connected, connected, disconnected) {
		return nil // We're closing an already closed connection
	}
	err := c.nc.Close()
	if err != nil {
		return ConnectionError{ConnectionID: c.id, Wrapped: err, message: "failed to close net.Conn"}
	}
	return nil
}

// put returns a connection to this pool. If the pool is connected, the connection is not
// expired, and there is space in the cache, the connection is returned to the cache.
func (p *pool) put(c *connection) error {
	if c.pool != p {
		return ErrWrongPool
	}
	if atomic.LoadInt32(&p.connected) != connected || c.expired() {
		return p.close(c)
	}

	// close the connection if the underlying pool is full
	if !p.conns.Put(c) {
		return p.close(c)
	}
	return nil
}
