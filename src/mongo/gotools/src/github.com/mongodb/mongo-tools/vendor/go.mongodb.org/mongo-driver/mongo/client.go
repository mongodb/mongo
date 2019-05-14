// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/event"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/auth"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/connection"
	"go.mongodb.org/mongo-driver/x/network/connstring"
	"go.mongodb.org/mongo-driver/x/network/description"
)

const defaultLocalThreshold = 15 * time.Millisecond

// Client performs operations on a given topology.
type Client struct {
	id              uuid.UUID
	topologyOptions []topology.Option
	topology        *topology.Topology
	connString      connstring.ConnString
	localThreshold  time.Duration
	retryWrites     bool
	clock           *session.ClusterClock
	readPreference  *readpref.ReadPref
	readConcern     *readconcern.ReadConcern
	writeConcern    *writeconcern.WriteConcern
	registry        *bsoncodec.Registry
	marshaller      BSONAppender
}

// Connect creates a new Client and then initializes it using the Connect method.
func Connect(ctx context.Context, opts ...*options.ClientOptions) (*Client, error) {
	c, err := NewClient(opts...)
	if err != nil {
		return nil, err
	}
	err = c.Connect(ctx)
	if err != nil {
		return nil, err
	}
	return c, nil
}

// NewClient creates a new client to connect to a cluster specified by the uri.
//
// When creating an options.ClientOptions, the order the methods are called matters. Later Set*
// methods will overwrite the values from previous Set* method invocations. This includes the
// ApplyURI method. This allows callers to determine the order of precedence for option
// application. For instance, if ApplyURI is called before SetAuth, the Credential from
// SetAuth will overwrite the values from the connection string. If ApplyURI is called
// after SetAuth, then its values will overwrite those from SetAuth.
//
// The opts parameter is processed using options.MergeClientOptions, which will overwrite entire
// option fields of previous options, there is no partial overwriting. For example, if Username is
// set in the Auth field for the first option, and Password is set for the second but with no
// Username, after the merge the Username field will be empty.
func NewClient(opts ...*options.ClientOptions) (*Client, error) {
	clientOpt := options.MergeClientOptions(opts...)

	id, err := uuid.New()
	if err != nil {
		return nil, err
	}
	client := &Client{id: id}

	err = client.configure(clientOpt)
	if err != nil {
		return nil, err
	}

	client.topology, err = topology.New(client.topologyOptions...)
	if err != nil {
		return nil, replaceErrors(err)
	}

	return client, nil
}

// Connect initializes the Client by starting background monitoring goroutines.
// This method must be called before a Client can be used.
func (c *Client) Connect(ctx context.Context) error {
	err := c.topology.Connect(ctx)
	if err != nil {
		return replaceErrors(err)
	}

	return nil

}

// Disconnect closes sockets to the topology referenced by this Client. It will
// shut down any monitoring goroutines, close the idle connection pool, and will
// wait until all the in use connections have been returned to the connection
// pool and closed before returning. If the context expires via cancellation,
// deadline, or timeout before the in use connections have returned, the in use
// connections will be closed, resulting in the failure of any in flight read
// or write operations. If this method returns with no errors, all connections
// associated with this Client have been closed.
func (c *Client) Disconnect(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}

	c.endSessions(ctx)
	return replaceErrors(c.topology.Disconnect(ctx))
}

// Ping verifies that the client can connect to the topology.
// If readPreference is nil then will use the client's default read
// preference.
func (c *Client) Ping(ctx context.Context, rp *readpref.ReadPref) error {
	if ctx == nil {
		ctx = context.Background()
	}

	if rp == nil {
		rp = c.readPreference
	}

	_, err := c.topology.SelectServer(ctx, description.ReadPrefSelector(rp))
	return replaceErrors(err)
}

// StartSession starts a new session.
func (c *Client) StartSession(opts ...*options.SessionOptions) (Session, error) {
	if c.topology.SessionPool == nil {
		return nil, ErrClientDisconnected
	}

	sopts := options.MergeSessionOptions(opts...)
	coreOpts := &session.ClientOptions{
		DefaultReadConcern:    c.readConcern,
		DefaultReadPreference: c.readPreference,
		DefaultWriteConcern:   c.writeConcern,
	}
	if sopts.CausalConsistency != nil {
		coreOpts.CausalConsistency = sopts.CausalConsistency
	}
	if sopts.DefaultReadConcern != nil {
		coreOpts.DefaultReadConcern = sopts.DefaultReadConcern
	}
	if sopts.DefaultWriteConcern != nil {
		coreOpts.DefaultWriteConcern = sopts.DefaultWriteConcern
	}
	if sopts.DefaultReadPreference != nil {
		coreOpts.DefaultReadPreference = sopts.DefaultReadPreference
	}

	sess, err := session.NewClientSession(c.topology.SessionPool, c.id, session.Explicit, coreOpts)
	if err != nil {
		return nil, replaceErrors(err)
	}

	sess.RetryWrite = c.retryWrites

	return &sessionImpl{
		Client: sess,
		topo:   c.topology,
	}, nil
}

func (c *Client) endSessions(ctx context.Context) {
	if c.topology.SessionPool == nil {
		return
	}
	cmd := command.EndSessions{
		Clock:      c.clock,
		SessionIDs: c.topology.SessionPool.IDSlice(),
	}

	_, _ = driverlegacy.EndSessions(ctx, cmd, c.topology, description.ReadPrefSelector(readpref.PrimaryPreferred()))
}

func (c *Client) configure(opts *options.ClientOptions) error {
	if err := opts.Validate(); err != nil {
		return err
	}

	var connOpts []connection.Option
	var serverOpts []topology.ServerOption
	var topologyOpts []topology.Option

	// TODO(GODRIVER-814): Add tests for topology, server, and connection related options.

	// AppName
	var appName string
	if opts.AppName != nil {
		appName = *opts.AppName
	}
	// Compressors & ZlibLevel
	var comps []string
	if len(opts.Compressors) > 0 {
		comps = opts.Compressors

		connOpts = append(connOpts, connection.WithCompressors(
			func(compressors []string) []string {
				return append(compressors, comps...)
			},
		))

		for _, comp := range comps {
			if comp == "zlib" {
				connOpts = append(connOpts, connection.WithZlibLevel(func(level *int) *int {
					return opts.ZlibLevel
				}))
			}
		}

		serverOpts = append(serverOpts, topology.WithCompressionOptions(
			func(opts ...string) []string { return append(opts, comps...) },
		))
	}
	// Handshaker
	var handshaker = func(connection.Handshaker) connection.Handshaker {
		return &command.Handshake{Client: command.ClientDoc(appName), Compressors: comps}
	}
	// Auth & Database & Password & Username
	if opts.Auth != nil {
		cred := &auth.Cred{
			Username:    opts.Auth.Username,
			Password:    opts.Auth.Password,
			PasswordSet: opts.Auth.PasswordSet,
			Props:       opts.Auth.AuthMechanismProperties,
			Source:      opts.Auth.AuthSource,
		}
		mechanism := opts.Auth.AuthMechanism

		if len(cred.Source) == 0 {
			switch strings.ToUpper(mechanism) {
			case auth.MongoDBX509, auth.GSSAPI, auth.PLAIN:
				cred.Source = "$external"
			default:
				cred.Source = "admin"
			}
		}

		authenticator, err := auth.CreateAuthenticator(mechanism, cred)
		if err != nil {
			return err
		}

		handshakeOpts := &auth.HandshakeOptions{
			AppName:       appName,
			Authenticator: authenticator,
			Compressors:   comps,
		}
		if mechanism == "" {
			// Required for SASL mechanism negotiation during handshake
			handshakeOpts.DBUser = cred.Source + "." + cred.Username
		}
		if opts.AuthenticateToAnything != nil && *opts.AuthenticateToAnything {
			// Authenticate arbiters
			handshakeOpts.PerformAuthentication = func(serv description.Server) bool {
				return true
			}
		}

		handshaker = func(connection.Handshaker) connection.Handshaker {
			return auth.Handshaker(nil, handshakeOpts)
		}
	}
	connOpts = append(connOpts, connection.WithHandshaker(handshaker))
	// ConnectTimeout
	if opts.ConnectTimeout != nil {
		serverOpts = append(serverOpts, topology.WithHeartbeatTimeout(
			func(time.Duration) time.Duration { return *opts.ConnectTimeout },
		))
		connOpts = append(connOpts, connection.WithConnectTimeout(
			func(time.Duration) time.Duration { return *opts.ConnectTimeout },
		))
	}
	// Dialer
	if opts.Dialer != nil {
		connOpts = append(connOpts, connection.WithDialer(
			func(connection.Dialer) connection.Dialer { return opts.Dialer },
		))
	}
	// Direct
	if opts.Direct != nil && *opts.Direct {
		topologyOpts = append(topologyOpts, topology.WithMode(
			func(topology.MonitorMode) topology.MonitorMode { return topology.SingleMode },
		))
	}
	// HeartbeatInterval
	if opts.HeartbeatInterval != nil {
		serverOpts = append(serverOpts, topology.WithHeartbeatInterval(
			func(time.Duration) time.Duration { return *opts.HeartbeatInterval },
		))
	}
	// Hosts
	hosts := []string{"localhost:27017"} // default host
	if len(opts.Hosts) > 0 {
		hosts = opts.Hosts
	}
	topologyOpts = append(topologyOpts, topology.WithSeedList(
		func(...string) []string { return hosts },
	))
	// LocalThreshold
	if opts.LocalThreshold != nil {
		c.localThreshold = *opts.LocalThreshold
	}
	// MaxConIdleTime
	if opts.MaxConnIdleTime != nil {
		connOpts = append(connOpts, connection.WithIdleTimeout(
			func(time.Duration) time.Duration { return *opts.MaxConnIdleTime },
		))
	}
	// MaxPoolSize
	if opts.MaxPoolSize != nil {
		serverOpts = append(
			serverOpts,
			topology.WithMaxConnections(func(uint16) uint16 { return *opts.MaxPoolSize }),
			topology.WithMaxIdleConnections(func(uint16) uint16 { return *opts.MaxPoolSize }),
		)
	}
	// Monitor
	if opts.Monitor != nil {
		connOpts = append(connOpts, connection.WithMonitor(
			func(*event.CommandMonitor) *event.CommandMonitor { return opts.Monitor },
		))
	}
	// ReadConcern
	c.readConcern = readconcern.New()
	if opts.ReadConcern != nil {
		c.readConcern = opts.ReadConcern
	}
	// ReadPreference
	c.readPreference = readpref.Primary()
	if opts.ReadPreference != nil {
		c.readPreference = opts.ReadPreference
	}
	// Registry
	c.registry = bson.DefaultRegistry
	if opts.Registry != nil {
		c.registry = opts.Registry
	}
	// ReplicaSet
	if opts.ReplicaSet != nil {
		topologyOpts = append(topologyOpts, topology.WithReplicaSetName(
			func(string) string { return *opts.ReplicaSet },
		))
	}
	// RetryWrites
	if opts.RetryWrites != nil {
		c.retryWrites = *opts.RetryWrites
	}
	// ServerSelectionTimeout
	if opts.ServerSelectionTimeout != nil {
		topologyOpts = append(topologyOpts, topology.WithServerSelectionTimeout(
			func(time.Duration) time.Duration { return *opts.ServerSelectionTimeout },
		))
	}
	// SocketTimeout
	if opts.SocketTimeout != nil {
		connOpts = append(
			connOpts,
			connection.WithReadTimeout(func(time.Duration) time.Duration { return *opts.SocketTimeout }),
			connection.WithWriteTimeout(func(time.Duration) time.Duration { return *opts.SocketTimeout }),
		)
	}
	// TLSConfig
	if opts.TLSConfig != nil {
		connOpts = append(connOpts, connection.WithTLSConfig(
			func(*connection.TLSConfig) *connection.TLSConfig {
				return &connection.TLSConfig{Config: opts.TLSConfig}
			},
		))
	}
	// WriteConcern
	if opts.WriteConcern != nil {
		c.writeConcern = opts.WriteConcern
	}

	// ClusterClock
	c.clock = new(session.ClusterClock)

	serverOpts = append(
		serverOpts,
		topology.WithClock(func(*session.ClusterClock) *session.ClusterClock { return c.clock }),
		topology.WithConnectionOptions(func(...connection.Option) []connection.Option { return connOpts }),
	)
	c.topologyOptions = append(topologyOpts, topology.WithServerOptions(
		func(...topology.ServerOption) []topology.ServerOption { return serverOpts },
	))

	return nil
}

// validSession returns an error if the session doesn't belong to the client
func (c *Client) validSession(sess *session.Client) error {
	if sess != nil && !uuid.Equal(sess.ClientID, c.id) {
		return ErrWrongClient
	}
	return nil
}

// Database returns a handle for a given database.
func (c *Client) Database(name string, opts ...*options.DatabaseOptions) *Database {
	return newDatabase(c, name, opts...)
}

// ListDatabases returns a ListDatabasesResult.
func (c *Client) ListDatabases(ctx context.Context, filter interface{}, opts ...*options.ListDatabasesOptions) (ListDatabasesResult, error) {
	if ctx == nil {
		ctx = context.Background()
	}

	sess := sessionFromContext(ctx)

	err := c.validSession(sess)
	if err != nil {
		return ListDatabasesResult{}, err
	}

	f, err := transformDocument(c.registry, filter)
	if err != nil {
		return ListDatabasesResult{}, err
	}

	cmd := command.ListDatabases{
		Filter:  f,
		Session: sess,
		Clock:   c.clock,
	}

	readSelector := description.CompositeSelector([]description.ServerSelector{
		description.ReadPrefSelector(readpref.Primary()),
		description.LatencySelector(c.localThreshold),
	})
	res, err := driverlegacy.ListDatabases(
		ctx, cmd,
		c.topology,
		readSelector,
		c.id,
		c.topology.SessionPool,
		opts...,
	)
	if err != nil {
		return ListDatabasesResult{}, replaceErrors(err)
	}

	return (ListDatabasesResult{}).fromResult(res), nil
}

// ListDatabaseNames returns a slice containing the names of all of the databases on the server.
func (c *Client) ListDatabaseNames(ctx context.Context, filter interface{}, opts ...*options.ListDatabasesOptions) ([]string, error) {
	opts = append(opts, options.ListDatabases().SetNameOnly(true))

	res, err := c.ListDatabases(ctx, filter, opts...)
	if err != nil {
		return nil, err
	}

	names := make([]string, 0)
	for _, spec := range res.Databases {
		names = append(names, spec.Name)
	}

	return names, nil
}

// WithSession allows a user to start a session themselves and manage
// its lifetime. The only way to provide a session to a CRUD method is
// to invoke that CRUD method with the mongo.SessionContext within the
// closure. The mongo.SessionContext can be used as a regular context,
// so methods like context.WithDeadline and context.WithTimeout are
// supported.
//
// If the context.Context already has a mongo.Session attached, that
// mongo.Session will be replaced with the one provided.
//
// Errors returned from the closure are transparently returned from
// this function.
func WithSession(ctx context.Context, sess Session, fn func(SessionContext) error) error {
	return fn(contextWithSession(ctx, sess))
}

// UseSession creates a default session, that is only valid for the
// lifetime of the closure. No cleanup outside of closing the session
// is done upon exiting the closure. This means that an outstanding
// transaction will be aborted, even if the closure returns an error.
//
// If ctx already contains a mongo.Session, that mongo.Session will be
// replaced with the newly created mongo.Session.
//
// Errors returned from the closure are transparently returned from
// this method.
func (c *Client) UseSession(ctx context.Context, fn func(SessionContext) error) error {
	return c.UseSessionWithOptions(ctx, options.Session(), fn)
}

// UseSessionWithOptions works like UseSession but allows the caller
// to specify the options used to create the session.
func (c *Client) UseSessionWithOptions(ctx context.Context, opts *options.SessionOptions, fn func(SessionContext) error) error {
	defaultSess, err := c.StartSession(opts)
	if err != nil {
		return err
	}

	defer defaultSess.EndSession(ctx)

	sessCtx := sessionContext{
		Context: context.WithValue(ctx, sessionKey{}, defaultSess),
		Session: defaultSess,
	}

	return fn(sessCtx)
}

// Watch returns a change stream cursor used to receive information of changes to the client. This method is preferred
// to running a raw aggregation with a $changeStream stage because it supports resumability in the case of some errors.
// The client must have read concern majority or no read concern for a change stream to be created successfully.
func (c *Client) Watch(ctx context.Context, pipeline interface{},
	opts ...*options.ChangeStreamOptions) (*ChangeStream, error) {
	if c.topology.SessionPool == nil {
		return nil, ErrClientDisconnected
	}

	return newClientChangeStream(ctx, c, pipeline, opts...)
}
