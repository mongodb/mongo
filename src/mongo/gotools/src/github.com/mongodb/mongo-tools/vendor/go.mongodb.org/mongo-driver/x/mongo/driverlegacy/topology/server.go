// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package topology

import (
	"context"
	"errors"
	"fmt"
	"math"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"go.mongodb.org/mongo-driver/event"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/auth"
	"go.mongodb.org/mongo-driver/x/network/address"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/connection"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
)

const minHeartbeatInterval = 500 * time.Millisecond
const connectionSemaphoreSize = math.MaxInt64

var isMasterOrRecoveringCodes = []int32{11600, 11602, 10107, 13435, 13436, 189, 91}

// ErrServerClosed occurs when an attempt to get a connection is made after
// the server has been closed.
var ErrServerClosed = errors.New("server is closed")

// ErrServerConnected occurs when at attempt to connect is made after a server
// has already been connected.
var ErrServerConnected = errors.New("server is connected")

// SelectedServer represents a specific server that was selected during server selection.
// It contains the kind of the topology it was selected from.
type SelectedServer struct {
	*Server

	Kind description.TopologyKind
}

// Description returns a description of the server as of the last heartbeat.
func (ss *SelectedServer) Description() description.SelectedServer {
	sdesc := ss.Server.Description()
	return description.SelectedServer{
		Server: sdesc,
		Kind:   ss.Kind,
	}
}

// These constants represent the connection states of a server.
const (
	disconnected int32 = iota
	disconnecting
	connected
	connecting
)

func connectionStateString(state int32) string {
	switch state {
	case 0:
		return "Disconnected"
	case 1:
		return "Disconnecting"
	case 2:
		return "Connected"
	case 3:
		return "Connecting"
	}

	return ""
}

// Server is a single server within a topology.
type Server struct {
	cfg     *serverConfig
	address address.Address

	connectionstate int32
	done            chan struct{}
	checkNow        chan struct{}
	closewg         sync.WaitGroup
	pool            connection.Pool

	desc atomic.Value // holds a description.Server

	averageRTTSet bool
	averageRTT    time.Duration

	subLock             sync.Mutex
	subscribers         map[uint64]chan description.Server
	currentSubscriberID uint64

	subscriptionsClosed bool

	updateTopologyCallback atomic.Value
}

// ConnectServer creates a new Server and then initializes it using the
// Connect method.
func ConnectServer(ctx context.Context, addr address.Address, topo func(description.Server), opts ...ServerOption) (*Server, error) {
	srvr, err := NewServer(addr, topo, opts...)
	if err != nil {
		return nil, err
	}
	err = srvr.Connect(ctx)
	if err != nil {
		return nil, err
	}
	return srvr, nil
}

// NewServer creates a new server. The mongodb server at the address will be monitored
// on an internal monitoring goroutine.
func NewServer(addr address.Address, topo func(description.Server), opts ...ServerOption) (*Server, error) {
	cfg, err := newServerConfig(opts...)
	if err != nil {
		return nil, err
	}

	s := &Server{
		cfg:     cfg,
		address: addr,

		done:     make(chan struct{}),
		checkNow: make(chan struct{}, 1),

		subscribers: make(map[uint64]chan description.Server),
	}
	s.desc.Store(description.Server{Addr: addr})
	s.updateTopologyCallback.Store(topo)

	var maxConns uint64
	if cfg.maxConns == 0 {
		maxConns = math.MaxInt64
	} else {
		maxConns = uint64(cfg.maxConns)
	}

	s.pool, err = connection.NewPool(addr, uint64(cfg.maxIdleConns), maxConns, cfg.connectionOpts...)
	if err != nil {
		return nil, err
	}

	return s, nil
}

// Connect initialzies the Server by starting background monitoring goroutines.
// This method must be called before a Server can be used.
func (s *Server) Connect(ctx context.Context) error {
	if !atomic.CompareAndSwapInt32(&s.connectionstate, disconnected, connected) {
		return ErrServerConnected
	}
	s.desc.Store(description.Server{Addr: s.address})
	go s.update()
	s.closewg.Add(1)
	return s.pool.Connect(ctx)
}

// Disconnect closes sockets to the server referenced by this Server.
// Subscriptions to this Server will be closed. Disconnect will shutdown
// any monitoring goroutines, close the idle connection pool, and will
// wait until all the in use connections have been returned to the connection
// pool and are closed before returning. If the context expires via
// cancellation, deadline, or timeout before the in use connections have been
// returned, the in use connections will be closed, resulting in the failure of
// any in flight read or write operations. If this method returns with no
// errors, all connections associated with this Server have been closed.
func (s *Server) Disconnect(ctx context.Context) error {
	if !atomic.CompareAndSwapInt32(&s.connectionstate, connected, disconnecting) {
		return ErrServerClosed
	}

	s.updateTopologyCallback.Store((func(description.Server))(nil))

	// For every call to Connect there must be at least 1 goroutine that is
	// waiting on the done channel.
	s.done <- struct{}{}
	err := s.pool.Disconnect(ctx)
	if err != nil {
		return err
	}

	s.closewg.Wait()
	atomic.StoreInt32(&s.connectionstate, disconnected)

	return nil
}

// Connection gets a connection to the server.
func (s *Server) Connection(ctx context.Context) (connection.Connection, error) {
	if atomic.LoadInt32(&s.connectionstate) != connected {
		return nil, ErrServerClosed
	}
	conn, desc, err := s.pool.Get(ctx)
	if err != nil {
		if _, ok := err.(*auth.Error); ok {
			// authentication error --> drain connection
			_ = s.pool.Drain()
		}
		if _, ok := err.(*connection.NetworkError); ok {
			// update description to unknown and clears the connection pool
			if desc != nil {
				desc.Kind = description.Unknown
				desc.LastError = err
				s.updateDescription(*desc, false)
			} else {
				_ = s.pool.Drain()
			}
		}
		return nil, err
	}
	if desc != nil {
		go s.updateDescription(*desc, false)
	}
	sc := &sconn{Connection: conn, s: s}
	return sc, nil
}

// Description returns a description of the server as of the last heartbeat.
func (s *Server) Description() description.Server {
	return s.desc.Load().(description.Server)
}

// SelectedDescription returns a description.SelectedServer with a Kind of
// Single. This can be used when performing tasks like monitoring a batch
// of servers and you want to run one off commands against those servers.
func (s *Server) SelectedDescription() description.SelectedServer {
	sdesc := s.Description()
	return description.SelectedServer{
		Server: sdesc,
		Kind:   description.Single,
	}
}

// Subscribe returns a ServerSubscription which has a channel on which all
// updated server descriptions will be sent. The channel will have a buffer
// size of one, and will be pre-populated with the current description.
func (s *Server) Subscribe() (*ServerSubscription, error) {
	if atomic.LoadInt32(&s.connectionstate) != connected {
		return nil, ErrSubscribeAfterClosed
	}
	ch := make(chan description.Server, 1)
	ch <- s.desc.Load().(description.Server)

	s.subLock.Lock()
	defer s.subLock.Unlock()
	if s.subscriptionsClosed {
		return nil, ErrSubscribeAfterClosed
	}
	id := s.currentSubscriberID
	s.subscribers[id] = ch
	s.currentSubscriberID++

	ss := &ServerSubscription{
		C:  ch,
		s:  s,
		id: id,
	}

	return ss, nil
}

// RequestImmediateCheck will cause the server to send a heartbeat immediately
// instead of waiting for the heartbeat timeout.
func (s *Server) RequestImmediateCheck() {
	select {
	case s.checkNow <- struct{}{}:
	default:
	}
}

// ProcessWriteConcernError checks if a WriteConcernError is an isNotMaster or
// isRecovering error, and if so updates the server accordingly.
func (s *Server) ProcessWriteConcernError(err *result.WriteConcernError) {
	if err == nil || !wceIsNotMasterOrRecovering(err) {
		return
	}
	desc := s.Description()
	desc.Kind = description.Unknown
	desc.LastError = err
	// updates description to unknown
	s.updateDescription(desc, false)
	s.RequestImmediateCheck()
	_ = s.pool.Drain()
}

func wceIsNotMasterOrRecovering(wce *result.WriteConcernError) bool {
	for _, code := range isMasterOrRecoveringCodes {
		if int32(wce.Code) == code {
			return true
		}
	}
	if strings.Contains(wce.ErrMsg, "not master") || strings.Contains(wce.ErrMsg, "node is recovering") {
		return true
	}
	return false
}

// update handles performing heartbeats and updating any subscribers of the
// newest description.Server retrieved.
func (s *Server) update() {
	defer s.closewg.Done()
	heartbeatTicker := time.NewTicker(s.cfg.heartbeatInterval)
	rateLimiter := time.NewTicker(minHeartbeatInterval)
	defer heartbeatTicker.Stop()
	defer rateLimiter.Stop()
	checkNow := s.checkNow
	done := s.done

	var doneOnce bool
	defer func() {
		if r := recover(); r != nil {
			if doneOnce {
				return
			}
			// We keep this goroutine alive attempting to read from the done channel.
			<-done
		}
	}()

	var conn connection.Connection
	var desc description.Server

	desc, conn = s.heartbeat(nil)
	s.updateDescription(desc, true)

	closeServer := func() {
		doneOnce = true
		s.subLock.Lock()
		for id, c := range s.subscribers {
			close(c)
			delete(s.subscribers, id)
		}
		s.subscriptionsClosed = true
		s.subLock.Unlock()
		if conn == nil {
			return
		}
		conn.Close()
	}
	for {
		select {
		case <-heartbeatTicker.C:
		case <-checkNow:
		case <-done:
			closeServer()
			return
		}

		select {
		case <-rateLimiter.C:
		case <-done:
			closeServer()
			return
		}

		desc, conn = s.heartbeat(conn)
		s.updateDescription(desc, false)
	}
}

// updateDescription handles updating the description on the Server, notifying
// subscribers, and potentially draining the connection pool. The initial
// parameter is used to determine if this is the first description from the
// server.
func (s *Server) updateDescription(desc description.Server, initial bool) {
	defer func() {
		//  ¯\_(ツ)_/¯
		_ = recover()
	}()
	s.desc.Store(desc)

	topo := s.updateTopologyCallback.Load().(func(description.Server))
	if topo != nil {
		topo(desc)
	}

	s.subLock.Lock()
	for _, c := range s.subscribers {
		select {
		// drain the channel if it isn't empty
		case <-c:
		default:
		}
		c <- desc
	}
	s.subLock.Unlock()

	if initial {
		// We don't clear the pool on the first update on the description.
		return
	}

	switch desc.Kind {
	case description.Unknown:
		_ = s.pool.Drain()
	}
}

// heartbeat sends a heartbeat to the server using the given connection. The connection can be nil.
func (s *Server) heartbeat(conn connection.Connection) (description.Server, connection.Connection) {
	const maxRetry = 2
	var saved error
	var desc description.Server
	var set bool
	var err error
	ctx := context.Background()

	for i := 1; i <= maxRetry; i++ {
		if conn != nil && conn.Expired() {
			conn.Close()
			conn = nil
		}

		if conn == nil {
			opts := []connection.Option{
				connection.WithConnectTimeout(func(time.Duration) time.Duration { return s.cfg.heartbeatTimeout }),
				connection.WithReadTimeout(func(time.Duration) time.Duration { return s.cfg.heartbeatTimeout }),
				connection.WithWriteTimeout(func(time.Duration) time.Duration { return s.cfg.heartbeatTimeout }),
			}
			opts = append(opts, s.cfg.connectionOpts...)
			// We override whatever handshaker is currently attached to the options with an empty
			// one because need to make sure we don't do auth.
			opts = append(opts, connection.WithHandshaker(func(h connection.Handshaker) connection.Handshaker {
				return nil
			}))

			// Override any command monitors specified in options with nil to avoid monitoring heartbeats.
			opts = append(opts, connection.WithMonitor(func(*event.CommandMonitor) *event.CommandMonitor {
				return nil
			}))
			conn, _, err = connection.New(ctx, s.address, opts...)
			if err != nil {
				saved = err
				if conn != nil {
					conn.Close()
				}
				conn = nil
				if _, ok := err.(*connection.NetworkError); ok {
					_ = s.pool.Drain()
					// If the server is not connected, give up and exit loop
					if s.Description().Kind == description.Unknown {
						break
					}
				}
				continue
			}
		}

		now := time.Now()

		isMasterCmd := &command.IsMaster{Compressors: s.cfg.compressionOpts}
		isMaster, err := isMasterCmd.RoundTrip(ctx, conn)
		// we do a retry if the server is connected, if succeed return new server desc (see below)
		if err != nil {
			saved = err
			conn.Close()
			conn = nil
			if _, ok := err.(connection.NetworkError); ok {
				_ = s.pool.Drain()
				// If the server is not connected, give up and exit loop
				if s.Description().Kind == description.Unknown {
					break
				}
			}
			continue
		}

		clusterTime := isMaster.ClusterTime
		if s.cfg.clock != nil {
			s.cfg.clock.AdvanceClusterTime(clusterTime)
		}

		delay := time.Since(now)
		desc = description.NewServer(s.address, isMaster).SetAverageRTT(s.updateAverageRTT(delay))
		desc.HeartbeatInterval = s.cfg.heartbeatInterval
		set = true

		break
	}

	if !set {
		desc = description.Server{
			Addr:      s.address,
			LastError: saved,
			Kind:      description.Unknown,
		}
	}

	return desc, conn
}

func (s *Server) updateAverageRTT(delay time.Duration) time.Duration {
	if !s.averageRTTSet {
		s.averageRTT = delay
	} else {
		alpha := 0.2
		s.averageRTT = time.Duration(alpha*float64(delay) + (1-alpha)*float64(s.averageRTT))
	}
	return s.averageRTT
}

// Drain will drain the connection pool of this server. This is mainly here so the
// pool for the server doesn't need to be directly exposed and so that when an error
// is returned from reading or writing, a client can drain the pool for this server.
// This is exposed here so we don't have to wrap the Connection type and sniff responses
// for errors that would cause the pool to be drained, which can in turn centralize the
// logic for handling errors in the Client type.
func (s *Server) Drain() error { return s.pool.Drain() }

// String implements the Stringer interface.
func (s *Server) String() string {
	desc := s.Description()
	connState := atomic.LoadInt32(&s.connectionstate)
	str := fmt.Sprintf("Addr: %s, Type: %s, State: %s",
		s.address, desc.Kind, connectionStateString(connState))
	if len(desc.Tags) != 0 {
		str += fmt.Sprintf(", Tag sets: %s", desc.Tags)
	}
	if connState == connected {
		str += fmt.Sprintf(", Average RTT: %d", desc.AverageRTT)
	}
	if desc.LastError != nil {
		str += fmt.Sprintf(", Last error: %s", desc.LastError)
	}

	return str
}

// ServerSubscription represents a subscription to the description.Server updates for
// a specific server.
type ServerSubscription struct {
	C  <-chan description.Server
	s  *Server
	id uint64
}

// Unsubscribe unsubscribes this ServerSubscription from updates and closes the
// subscription channel.
func (ss *ServerSubscription) Unsubscribe() error {
	ss.s.subLock.Lock()
	defer ss.s.subLock.Unlock()
	if ss.s.subscriptionsClosed {
		return nil
	}

	ch, ok := ss.s.subscribers[ss.id]
	if !ok {
		return nil
	}

	close(ch)
	delete(ss.s.subscribers, ss.id)

	return nil
}
