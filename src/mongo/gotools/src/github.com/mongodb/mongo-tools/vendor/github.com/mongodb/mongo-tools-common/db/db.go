// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"context"
	"errors"
	"time"

	"go.mongodb.org/mongo-driver/x/network/connection"

	"github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/password"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo"
	mopt "go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"

	"fmt"
	"io"
	"strings"
	"sync"
)

type (
	sessionFlag uint32
)

// Session flags.
const (
	None      sessionFlag = 0
	Monotonic sessionFlag = 1 << iota
	DisableSocketTimeout
)

// MongoDB enforced limits.
const (
	MaxBSONSize = 16 * 1024 * 1024 // 16MB - maximum BSON document size
)

// Default port for integration tests
const (
	DefaultTestPort = "33333"
)

// Hard coded socket timeout in seconds
const SocketTimeout = 600

const (
	ErrLostConnection     = "lost connection to server"
	ErrNoReachableServers = "no reachable servers"
	ErrNsNotFound         = "ns not found"
	// replication errors list the replset name if we are talking to a mongos,
	// so we can only check for this universal prefix
	ErrReplTimeoutPrefix            = "waiting for replication timed out"
	ErrCouldNotContactPrimaryPrefix = "could not contact primary for replica set"
	ErrWriteResultsUnavailable      = "write results unavailable from"
	ErrCouldNotFindPrimaryPrefix    = `could not find host matching read preference { mode: "primary"`
	ErrUnableToTargetPrefix         = "unable to target"
	ErrNotMaster                    = "not master"
	ErrConnectionRefusedSuffix      = "Connection refused"
)

// Used to manage database sessions
type SessionProvider struct {
	sync.Mutex

	// the master client used for operations
	client *mongo.Client
}

// ApplyOpsResponse represents the response from an 'applyOps' command.
type ApplyOpsResponse struct {
	Ok     bool   `bson:"ok"`
	ErrMsg string `bson:"errmsg"`
}

// Oplog represents a MongoDB oplog document.
type Oplog struct {
	Timestamp primitive.Timestamp `bson:"ts"`
	HistoryID int64               `bson:"h"`
	Version   int                 `bson:"v"`
	Operation string              `bson:"op"`
	Namespace string              `bson:"ns"`
	Object    bson.D              `bson:"o"`
	Query     bson.D              `bson:"o2"`
	UI        *primitive.Binary   `bson:"ui,omitempty"`
}

// Returns a mongo.Client connected to the database server for which the
// session provider is configured.
func (sp *SessionProvider) GetSession() (*mongo.Client, error) {
	sp.Lock()
	defer sp.Unlock()

	if sp.client == nil {
		return nil, errors.New("SessionProvider already closed")
	}

	return sp.client, nil
}

// Close closes the master session in the connection pool
func (sp *SessionProvider) Close() {
	sp.Lock()
	defer sp.Unlock()
	if sp.client != nil {
		_ = sp.client.Disconnect(context.Background())
		sp.client = nil
	}
}

// DB provides a database with the default read preference
func (sp *SessionProvider) DB(name string) *mongo.Database {
	return sp.client.Database(name)
}

// NewSessionProvider constructs a session provider, including a connected client.
func NewSessionProvider(opts options.ToolOptions) (*SessionProvider, error) {
	// finalize auth options, filling in missing passwords
	if opts.Auth.ShouldAskForPassword() {
		opts.Auth.Password = password.Prompt()
	}

	client, err := configureClient(opts)
	if err != nil {
		return nil, fmt.Errorf("error configuring the connector: %v", err)
	}
	err = client.Connect(context.Background())
	if err != nil {
		return nil, err
	}
	err = client.Ping(context.Background(), nil)
	if err != nil {
		return nil, fmt.Errorf("could not connect to server: %v", err)
	}

	// create the provider
	return &SessionProvider{client: client}, nil
}

// configure the client according to the options set in the uri and in the provided ToolOptions, with ToolOptions having precedence.
func configureClient(opts options.ToolOptions) (*mongo.Client, error) {
	clientopt := mopt.Client()

	if opts.URI == nil || opts.URI.ConnectionString == "" {
		// XXX Normal operations shouldn't ever reach here because a URI should
		// be created in options parsing, but tests still manually construct
		// options and generally don't construct a URI, so we invoke the URI
		// normalization routine here to correct for that.
		opts.NormalizeHostPortURI()
	}

	uriOpts := mopt.Client().ApplyURI(opts.URI.ConnectionString)
	if err := uriOpts.Validate(); err != nil {
		return nil, fmt.Errorf("error parsing options from URI: %v", err)
	}
	timeout := time.Duration(opts.Timeout) * time.Second

	clientopt.SetConnectTimeout(timeout)
	clientopt.SetSocketTimeout(SocketTimeout * time.Second)
	clientopt.SetReplicaSet(opts.ReplicaSetName)

	clientopt.SetAppName(opts.AppName)
	clientopt.SetDirect(opts.Direct)
	if opts.ReadPreference != nil {
		clientopt.SetReadPreference(opts.ReadPreference)
	}
	if opts.WriteConcern != nil {
		clientopt.SetWriteConcern(opts.WriteConcern)
	} else {
		// If no write concern was specified, default to majority
		clientopt.SetWriteConcern(writeconcern.New(writeconcern.WMajority()))
	}

	if opts.Auth != nil && opts.Auth.IsSet() {
		cred := mopt.Credential{
			Username:      opts.Auth.Username,
			Password:      opts.Auth.Password,
			AuthSource:    opts.GetAuthenticationDatabase(),
			AuthMechanism: opts.Auth.Mechanism,
		}
		// Technically, an empty password is possible, but the tools don't have the
		// means to easily distinguish and so require a non-empty password.
		if cred.Password != "" {
			cred.PasswordSet = true
		}
		if opts.Kerberos != nil && cred.AuthMechanism == "GSSAPI" {
			props := make(map[string]string)
			if opts.Kerberos.Service != "" {
				props["SERVICE_NAME"] = opts.Kerberos.Service
			}
			// XXX How do we use opts.Kerberos.ServiceHost if at all?
			cred.AuthMechanismProperties = props
		}
		clientopt.SetAuth(cred)
	}

	if opts.SSL != nil && opts.UseSSL {
		// Error on unsupported features
		if opts.SSLFipsMode {
			return nil, fmt.Errorf("FIPS mode not supported")
		}
		if opts.SSLCRLFile != "" {
			return nil, fmt.Errorf("CRL files are not supported on this platform")
		}

		tlsConfig := connection.NewTLSConfig()
		if opts.SSLAllowInvalidCert || opts.SSLAllowInvalidHost {
			tlsConfig.SetInsecure(true)
		}
		if opts.SSLPEMKeyFile != "" {
			if opts.SSLPEMKeyPassword != "" {
				tlsConfig.SetClientCertDecryptPassword(func() string { return opts.SSLPEMKeyPassword })
			}

			_, err := tlsConfig.AddClientCertFromFile(opts.SSLPEMKeyFile)
			if err != nil {
				return nil, fmt.Errorf("error configuring client, can't load client certificate: %v", err)
			}
		}
		if opts.SSLCAFile != "" {
			if err := tlsConfig.AddCACertFromFile(opts.SSLCAFile); err != nil {
				return nil, fmt.Errorf("error configuring client, can't load CA file: %v", err)
			}
		}
		clientopt.SetTLSConfig(tlsConfig.Config)
	}

	return mongo.NewClient(uriOpts, clientopt)
}

// IsConnectionError returns a boolean indicating if a given error is due to
// an error in an underlying DB connection (as opposed to some other write
// failure such as a duplicate key error)
func IsConnectionError(err error) bool {
	if err == nil {
		return false
	}

	// The new driver stringifies command errors as "(Name) Message" rather than just "message". Cast to the
	// CommandError type if possible to extract the correct error message.
	errMsg := err.Error()
	if cmdErr, ok := err.(mongo.CommandError); ok {
		errMsg = cmdErr.Message
	}

	lowerCaseError := strings.ToLower(errMsg)
	if lowerCaseError == ErrNoReachableServers ||
		err == io.EOF ||
		strings.Contains(lowerCaseError, ErrReplTimeoutPrefix) ||
		strings.Contains(lowerCaseError, ErrCouldNotContactPrimaryPrefix) ||
		strings.Contains(lowerCaseError, ErrWriteResultsUnavailable) ||
		strings.Contains(lowerCaseError, ErrCouldNotFindPrimaryPrefix) ||
		strings.Contains(lowerCaseError, ErrUnableToTargetPrefix) ||
		strings.Contains(lowerCaseError, ErrNotMaster) ||
		strings.HasSuffix(lowerCaseError, ErrConnectionRefusedSuffix) {
		return true
	}
	return false
}
