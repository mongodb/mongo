// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package connection

import (
	"context"
	"sync"
	"sync/atomic"

	"go.mongodb.org/mongo-driver/x/network/address"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
	"golang.org/x/sync/semaphore"
)

// ErrPoolClosed is returned from an attempt to use a closed pool.
var ErrPoolClosed = PoolError("pool is closed")

// ErrSizeLargerThanCapacity is returned from an attempt to create a pool with a size
// larger than the capacity.
var ErrSizeLargerThanCapacity = PoolError("size is larger than capacity")

// ErrPoolConnected is returned from an attempt to connect an already connected pool
var ErrPoolConnected = PoolError("pool is connected")

// ErrPoolDisconnected is returned from an attempt to disconnect an already disconnected
// or disconnecting pool.
var ErrPoolDisconnected = PoolError("pool is disconnected or disconnecting")

// ErrConnectionClosed is returned from an attempt to use an already closed connection.
var ErrConnectionClosed = Error{ConnectionID: "<closed>", message: "connection is closed"}

// These constants represent the connection states of a pool.
const (
	disconnected int32 = iota
	disconnecting
	connected
)

// Pool is used to pool Connections to a server.
type Pool interface {
	// Get must return a nil *description.Server if the returned connection is
	// not a newly dialed connection.
	Get(context.Context) (Connection, *description.Server, error)
	// Connect handles the initialization of a Pool and allow Connections to be
	// retrieved and pooled. Implementations must return an error if Connect is
	// called more than once before calling Disconnect.
	Connect(context.Context) error
	// Disconnect closest connections managed by this Pool. Implementations must
	// either wait until all of the connections in use have been returned and
	// closed or the context expires before returning. If the context expires
	// via cancellation, deadline, timeout, or some other manner, implementations
	// must close the in use connections. If this method returns with no errors,
	// all connections managed by this pool must be closed. Calling Disconnect
	// multiple times after a single Connect call must result in an error.
	Disconnect(context.Context) error
	Drain() error
}

type pool struct {
	address    address.Address
	opts       []Option
	conns      chan *pooledConnection
	generation uint64
	sem        *semaphore.Weighted
	connected  int32
	nextid     uint64
	capacity   uint64
	inflight   map[uint64]*pooledConnection

	sync.Mutex
}

// NewPool creates a new pool that will hold size number of idle connections
// and will create a max of capacity connections. It will use the provided
// options.
func NewPool(addr address.Address, size, capacity uint64, opts ...Option) (Pool, error) {
	if size > capacity {
		return nil, ErrSizeLargerThanCapacity
	}
	p := &pool{
		address:    addr,
		conns:      make(chan *pooledConnection, size),
		generation: 0,
		sem:        semaphore.NewWeighted(int64(capacity)),
		connected:  disconnected,
		capacity:   capacity,
		inflight:   make(map[uint64]*pooledConnection),
		opts:       opts,
	}
	return p, nil
}

func (p *pool) Drain() error {
	atomic.AddUint64(&p.generation, 1)
	return nil
}

func (p *pool) Connect(ctx context.Context) error {
	if !atomic.CompareAndSwapInt32(&p.connected, disconnected, connected) {
		return ErrPoolConnected
	}
	atomic.AddUint64(&p.generation, 1)
	return nil
}

func (p *pool) Disconnect(ctx context.Context) error {
	if !atomic.CompareAndSwapInt32(&p.connected, connected, disconnecting) {
		return ErrPoolDisconnected
	}

	// We first clear out the idle connections, then we attempt to acquire the entire capacity
	// semaphore. If the context is either cancelled, the deadline expires, or there is a timeout
	// the semaphore acquire method will return an error. If that happens, we will aggressively
	// close the remaining open connections. If we were able to successfully acquire the semaphore,
	// then all of the in flight connections have been closed and we release the semaphore.
loop:
	for {
		select {
		case pc := <-p.conns:
			// This error would be overwritten by the semaphore
			_ = p.closeConnection(pc)
		default:
			break loop
		}
	}
	err := p.sem.Acquire(ctx, int64(p.capacity))
	if err != nil {
		p.Lock()
		// We copy the remaining connections to close into a slice, then
		// iterate the slice to do the closing. This allows us to use a single
		// function to actually clean up and close connections at the expense of
		// a double iteration in the worst case.
		toClose := make([]*pooledConnection, 0, len(p.inflight))
		for _, pc := range p.inflight {
			toClose = append(toClose, pc)
		}
		p.Unlock()
		for _, pc := range toClose {
			_ = pc.Close()
		}
	} else {
		p.sem.Release(int64(p.capacity))
	}
	atomic.StoreInt32(&p.connected, disconnected)
	return nil
}

func (p *pool) Get(ctx context.Context) (Connection, *description.Server, error) {
	if atomic.LoadInt32(&p.connected) != connected {
		return nil, nil, ErrPoolClosed
	}

	err := p.sem.Acquire(ctx, 1)
	if err != nil {
		return nil, nil, err
	}

	return p.get(ctx)
}

func (p *pool) get(ctx context.Context) (Connection, *description.Server, error) {
	g := atomic.LoadUint64(&p.generation)
	select {
	case c := <-p.conns:
		if c.Expired() {
			go p.closeConnection(c)
			return p.get(ctx)
		}

		return &acquired{Connection: c, sem: p.sem}, nil, nil
	case <-ctx.Done():
		p.sem.Release(1)
		return nil, nil, ctx.Err()
	default:
		c, desc, err := New(ctx, p.address, p.opts...)
		if err != nil {
			p.sem.Release(1)
			return nil, nil, err
		}

		pc := &pooledConnection{
			Connection: c,
			p:          p,
			generation: g,
			id:         atomic.AddUint64(&p.nextid, 1),
		}
		p.Lock()
		if atomic.LoadInt32(&p.connected) != connected {
			p.Unlock()
			p.sem.Release(1)
			p.closeConnection(pc)
			return nil, nil, ErrPoolClosed
		}
		defer p.Unlock()
		p.inflight[pc.id] = pc
		return &acquired{Connection: pc, sem: p.sem}, desc, nil
	}
}

func (p *pool) closeConnection(pc *pooledConnection) error {
	if !atomic.CompareAndSwapInt32(&pc.closed, 0, 1) {
		return nil
	}
	p.Lock()
	delete(p.inflight, pc.id)
	p.Unlock()
	return pc.Connection.Close()
}

func (p *pool) returnConnection(pc *pooledConnection) error {
	if atomic.LoadInt32(&p.connected) != connected || pc.Expired() {
		return p.closeConnection(pc)
	}

	select {
	case p.conns <- pc:
		return nil
	default:
		return p.closeConnection(pc)
	}
}

func (p *pool) isExpired(generation uint64) bool {
	return generation < atomic.LoadUint64(&p.generation)
}

type pooledConnection struct {
	Connection
	p          *pool
	generation uint64
	id         uint64
	closed     int32
}

func (pc *pooledConnection) Close() error {
	return pc.p.returnConnection(pc)
}

func (pc *pooledConnection) Expired() bool {
	return pc.Connection.Expired() || pc.p.isExpired(pc.generation)
}

type acquired struct {
	Connection

	sem *semaphore.Weighted
	sync.Mutex
}

func (a *acquired) WriteWireMessage(ctx context.Context, wm wiremessage.WireMessage) error {
	a.Lock()
	defer a.Unlock()
	if a.Connection == nil {
		return ErrConnectionClosed
	}
	return a.Connection.WriteWireMessage(ctx, wm)
}

func (a *acquired) ReadWireMessage(ctx context.Context) (wiremessage.WireMessage, error) {
	a.Lock()
	defer a.Unlock()
	if a.Connection == nil {
		return nil, ErrConnectionClosed
	}
	return a.Connection.ReadWireMessage(ctx)
}

func (a *acquired) Close() error {
	a.Lock()
	defer a.Unlock()
	if a.Connection == nil {
		return nil
	}
	err := a.Connection.Close()
	a.sem.Release(1)
	a.Connection = nil
	return err
}

func (a *acquired) Expired() bool {
	a.Lock()
	defer a.Unlock()
	if a.Connection == nil {
		return true
	}
	return a.Connection.Expired()
}

func (a *acquired) Alive() bool {
	a.Lock()
	defer a.Unlock()
	if a.Connection == nil {
		return false
	}
	return a.Connection.Alive()
}

func (a *acquired) ID() string {
	a.Lock()
	defer a.Unlock()
	if a.Connection == nil {
		return "<closed>"
	}
	return a.Connection.ID()
}
