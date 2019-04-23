// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package topology contains types that handles the discovery, monitoring, and selection
// of servers. This package is designed to expose enough inner workings of service discovery
// and monitoring to allow low level applications to have fine grained control, while hiding
// most of the detailed implementation of the algorithms.
package topology // import "go.mongodb.org/mongo-driver/x/mongo/driver/topology"

import (
	"context"
	"errors"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"

	"fmt"

	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/network/address"
	"go.mongodb.org/mongo-driver/x/network/description"
)

// ErrSubscribeAfterClosed is returned when a user attempts to subscribe to a
// closed Server or Topology.
var ErrSubscribeAfterClosed = errors.New("cannot subscribe after close")

// ErrTopologyClosed is returned when a user attempts to call a method on a
// closed Topology.
var ErrTopologyClosed = errors.New("topology is closed")

// ErrTopologyConnected is returned whena  user attempts to connect to an
// already connected Topology.
var ErrTopologyConnected = errors.New("topology is connected or connecting")

// ErrServerSelectionTimeout is returned from server selection when the server
// selection process took longer than allowed by the timeout.
var ErrServerSelectionTimeout = errors.New("server selection timeout")

// MonitorMode represents the way in which a server is monitored.
type MonitorMode uint8

// These constants are the available monitoring modes.
const (
	AutomaticMode MonitorMode = iota
	SingleMode
)

// Topology represents a MongoDB deployment.
type Topology struct {
	registry *bsoncodec.Registry

	connectionstate int32

	cfg *config

	desc atomic.Value // holds a description.Topology

	done chan struct{}

	fsm       *fsm
	changes   chan description.Server
	changeswg sync.WaitGroup

	SessionPool *session.Pool

	// This should really be encapsulated into it's own type. This will likely
	// require a redesign so we can share a minimum of data between the
	// subscribers and the topology.
	subscribers         map[uint64]chan description.Topology
	currentSubscriberID uint64
	subscriptionsClosed bool
	subLock             sync.Mutex

	// We should redesign how we connect and handle individal servers. This is
	// too difficult to maintain and it's rather easy to accidentally access
	// the servers without acquiring the lock or checking if the servers are
	// closed. This lock should also be an RWMutex.
	serversLock   sync.Mutex
	serversClosed bool
	servers       map[address.Address]*Server

	wg sync.WaitGroup
}

// New creates a new topology.
func New(opts ...Option) (*Topology, error) {
	cfg, err := newConfig(opts...)
	if err != nil {
		return nil, err
	}

	t := &Topology{
		cfg:         cfg,
		done:        make(chan struct{}),
		fsm:         newFSM(),
		changes:     make(chan description.Server),
		subscribers: make(map[uint64]chan description.Topology),
		servers:     make(map[address.Address]*Server),
	}
	t.desc.Store(description.Topology{})

	if cfg.replicaSetName != "" {
		t.fsm.SetName = cfg.replicaSetName
		t.fsm.Kind = description.ReplicaSetNoPrimary
	}

	if cfg.mode == SingleMode {
		t.fsm.Kind = description.Single
	}

	return t, nil
}

// Connect initializes a Topology and starts the monitoring process. This function
// must be called to properly monitor the topology.
func (t *Topology) Connect(ctx context.Context) error {
	if !atomic.CompareAndSwapInt32(&t.connectionstate, disconnected, connecting) {
		return ErrTopologyConnected
	}

	t.desc.Store(description.Topology{})
	var err error
	t.serversLock.Lock()
	for _, a := range t.cfg.seedList {
		addr := address.Address(a).Canonicalize()
		t.fsm.Servers = append(t.fsm.Servers, description.Server{Addr: addr})
		err = t.addServer(ctx, addr)
	}
	t.serversLock.Unlock()

	go t.update()
	t.changeswg.Add(1)

	t.subscriptionsClosed = false // explicitly set in case topology was disconnected and then reconnected

	atomic.StoreInt32(&t.connectionstate, connected)

	// After connection, make a subscription to keep the pool updated
	sub, err := t.Subscribe()
	t.SessionPool = session.NewPool(sub.C)
	return err
}

// Disconnect closes the topology. It stops the monitoring thread and
// closes all open subscriptions.
func (t *Topology) Disconnect(ctx context.Context) error {
	if !atomic.CompareAndSwapInt32(&t.connectionstate, connected, disconnecting) {
		return ErrTopologyClosed
	}

	t.serversLock.Lock()
	t.serversClosed = true
	for addr, server := range t.servers {
		t.removeServer(ctx, addr, server)
	}
	t.serversLock.Unlock()

	t.wg.Wait()
	t.done <- struct{}{}
	t.changeswg.Wait()

	t.desc.Store(description.Topology{})

	atomic.StoreInt32(&t.connectionstate, disconnected)
	return nil
}

// Description returns a description of the topology.
func (t *Topology) Description() description.Topology {
	td, ok := t.desc.Load().(description.Topology)
	if !ok {
		td = description.Topology{}
	}
	return td
}

// Subscribe returns a Subscription on which all updated description.Topologys
// will be sent. The channel of the subscription will have a buffer size of one,
// and will be pre-populated with the current description.Topology.
func (t *Topology) Subscribe() (*Subscription, error) {
	if atomic.LoadInt32(&t.connectionstate) != connected {
		return nil, errors.New("cannot subscribe to Topology that is not connected")
	}
	ch := make(chan description.Topology, 1)
	td, ok := t.desc.Load().(description.Topology)
	if !ok {
		td = description.Topology{}
	}
	ch <- td

	t.subLock.Lock()
	defer t.subLock.Unlock()
	if t.subscriptionsClosed {
		return nil, ErrSubscribeAfterClosed
	}
	id := t.currentSubscriberID
	t.subscribers[id] = ch
	t.currentSubscriberID++

	return &Subscription{
		C:  ch,
		t:  t,
		id: id,
	}, nil
}

// RequestImmediateCheck will send heartbeats to all the servers in the
// topology right away, instead of waiting for the heartbeat timeout.
func (t *Topology) RequestImmediateCheck() {
	if atomic.LoadInt32(&t.connectionstate) != connected {
		return
	}
	t.serversLock.Lock()
	for _, server := range t.servers {
		server.RequestImmediateCheck()
	}
	t.serversLock.Unlock()
}

// SupportsSessions returns true if the topology supports sessions.
func (t *Topology) SupportsSessions() bool {
	return t.Description().SessionTimeoutMinutes != 0 && t.Description().Kind != description.Single
}

// SelectServer selects a server given a selector.SelectServer complies with the
// server selection spec, and will time out after severSelectionTimeout or when the
// parent context is done.
func (t *Topology) SelectServer(ctx context.Context, ss description.ServerSelector) (*SelectedServer, error) {
	if atomic.LoadInt32(&t.connectionstate) != connected {
		return nil, ErrTopologyClosed
	}
	var ssTimeoutCh <-chan time.Time

	if t.cfg.serverSelectionTimeout > 0 {
		ssTimeout := time.NewTimer(t.cfg.serverSelectionTimeout)
		ssTimeoutCh = ssTimeout.C
		defer ssTimeout.Stop()
	}

	sub, err := t.Subscribe()
	if err != nil {
		return nil, err
	}
	defer sub.Unsubscribe()

	for {
		suitable, err := t.selectServer(ctx, sub.C, ss, ssTimeoutCh)
		if err != nil {
			return nil, err
		}

		selected := suitable[rand.Intn(len(suitable))]
		selectedS, err := t.FindServer(selected)
		switch {
		case err != nil:
			return nil, err
		case selectedS != nil:
			return selectedS, nil
		default:
			// We don't have an actual server for the provided description.
			// This could happen for a number of reasons, including that the
			// server has since stopped being a part of this topology, or that
			// the server selector returned no suitable servers.
		}
	}
}

// FindServer will attempt to find a server that fits the given server description.
// This method will return nil, nil if a matching server could not be found.
func (t *Topology) FindServer(selected description.Server) (*SelectedServer, error) {
	if atomic.LoadInt32(&t.connectionstate) != connected {
		return nil, ErrTopologyClosed
	}
	t.serversLock.Lock()
	defer t.serversLock.Unlock()
	server, ok := t.servers[selected.Addr]
	if !ok {
		return nil, nil
	}

	desc := t.Description()
	return &SelectedServer{
		Server: server,
		Kind:   desc.Kind,
	}, nil
}

func wrapServerSelectionError(err error, t *Topology) error {
	return fmt.Errorf("server selection error: %v\ncurrent topology: %s", err, t.String())
}

// selectServer is the core piece of server selection. It handles getting
// topology descriptions and running sever selection on those descriptions.
func (t *Topology) selectServer(ctx context.Context, subscriptionCh <-chan description.Topology, ss description.ServerSelector, timeoutCh <-chan time.Time) ([]description.Server, error) {
	var current description.Topology
	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-timeoutCh:
			return nil, wrapServerSelectionError(ErrServerSelectionTimeout, t)
		case current = <-subscriptionCh:
		}

		var allowed []description.Server
		for _, s := range current.Servers {
			if s.Kind != description.Unknown {
				allowed = append(allowed, s)
			}
		}

		suitable, err := ss.SelectServer(current, allowed)
		if err != nil {
			return nil, wrapServerSelectionError(err, t)
		}

		if len(suitable) > 0 {
			return suitable, nil
		}

		t.RequestImmediateCheck()
	}
}

func (t *Topology) update() {
	defer t.changeswg.Done()
	defer func() {
		//  ¯\_(ツ)_/¯
		if r := recover(); r != nil {
			<-t.done
		}
	}()

	for {
		select {
		case change := <-t.changes:
			current, err := t.apply(context.TODO(), change)
			if err != nil {
				continue
			}

			t.desc.Store(current)
			t.subLock.Lock()
			for _, ch := range t.subscribers {
				// We drain the description if there's one in the channel
				select {
				case <-ch:
				default:
				}
				ch <- current
			}
			t.subLock.Unlock()
		case <-t.done:
			t.subLock.Lock()
			for id, ch := range t.subscribers {
				close(ch)
				delete(t.subscribers, id)
			}
			t.subscriptionsClosed = true
			t.subLock.Unlock()
			return
		}
	}
}

func (t *Topology) apply(ctx context.Context, desc description.Server) (description.Topology, error) {
	var err error
	prev := t.fsm.Topology

	current, err := t.fsm.apply(desc)
	if err != nil {
		return description.Topology{}, err
	}

	diff := description.DiffTopology(prev, current)
	t.serversLock.Lock()
	if t.serversClosed {
		t.serversLock.Unlock()
		return description.Topology{}, nil
	}

	for _, removed := range diff.Removed {
		if s, ok := t.servers[removed.Addr]; ok {
			t.removeServer(ctx, removed.Addr, s)
		}
	}

	for _, added := range diff.Added {
		_ = t.addServer(ctx, added.Addr)
	}
	t.serversLock.Unlock()
	return current, nil
}

func (t *Topology) addServer(ctx context.Context, addr address.Address) error {
	if _, ok := t.servers[addr]; ok {
		return nil
	}

	svr, err := ConnectServer(ctx, addr, t.cfg.serverOpts...)
	if err != nil {
		return err
	}

	t.servers[addr] = svr
	var sub *ServerSubscription
	sub, err = svr.Subscribe()
	if err != nil {
		return err
	}

	t.wg.Add(1)
	go func() {
		for c := range sub.C {
			t.changes <- c
		}

		t.wg.Done()
	}()

	return nil
}

func (t *Topology) removeServer(ctx context.Context, addr address.Address, server *Server) {
	_ = server.Disconnect(ctx)
	delete(t.servers, addr)
}

// String implements the Stringer interface
func (t *Topology) String() string {
	desc := t.Description()
	str := fmt.Sprintf("Type: %s\nServers:\n", desc.Kind)
	for _, s := range t.servers {
		str += s.String() + "\n"
	}
	return str
}

// Subscription is a subscription to updates to the description of the Topology that created this
// Subscription.
type Subscription struct {
	C  <-chan description.Topology
	t  *Topology
	id uint64
}

// Unsubscribe unsubscribes this Subscription from updates and closes the
// subscription channel.
func (s *Subscription) Unsubscribe() error {
	s.t.subLock.Lock()
	defer s.t.subLock.Unlock()
	if s.t.subscriptionsClosed {
		return nil
	}

	ch, ok := s.t.subscribers[s.id]
	if !ok {
		return nil
	}

	close(ch)
	delete(s.t.subscribers, s.id)

	return nil
}
