// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"io/ioutil"
	"strings"
	"sync"
	"time"

	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/password"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	mopt "go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
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

	// ignorable errors
	ErrDuplicateKeyCode         = 11000
	ErrFailedDocumentValidation = 121
	ErrUnacknowledgedWrite      = "unacknowledged write"
)

var ignorableWriteErrorCodes = map[int]bool{ErrDuplicateKeyCode: true, ErrFailedDocumentValidation: true}

const (
	continueThroughErrorFormat = "continuing through error: %v"
)

// Used to manage database sessions
type SessionProvider struct {
	sync.Mutex

	// the master client used for operations
	client *mongo.Client
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
		pass, err := password.Prompt()
		if err != nil {
			return nil, fmt.Errorf("error reading password: %v", err)
		}
		opts.Auth.Password = pass
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

// addClientCertFromFile adds a client certificate to the configuration given a path to the
// containing file and returns the certificate's subject name.
func addClientCertFromFile(cfg *tls.Config, clientFile, keyPasswd string) (string, error) {
	data, err := ioutil.ReadFile(clientFile)
	if err != nil {
		return "", err
	}

	var currentBlock *pem.Block
	var certBlock, certDecodedBlock, keyBlock []byte

	remaining := data
	start := 0
	for {
		currentBlock, remaining = pem.Decode(remaining)
		if currentBlock == nil {
			break
		}

		if currentBlock.Type == "CERTIFICATE" {
			certBlock = data[start : len(data)-len(remaining)]
			certDecodedBlock = currentBlock.Bytes
			start += len(certBlock)
		} else if strings.HasSuffix(currentBlock.Type, "PRIVATE KEY") {
			if keyPasswd != "" && x509.IsEncryptedPEMBlock(currentBlock) {
				var encoded bytes.Buffer
				buf, err := x509.DecryptPEMBlock(currentBlock, []byte(keyPasswd))
				if err != nil {
					return "", err
				}

				pem.Encode(&encoded, &pem.Block{Type: currentBlock.Type, Bytes: buf})
				keyBlock = encoded.Bytes()
				start = len(data) - len(remaining)
			} else {
				keyBlock = data[start : len(data)-len(remaining)]
				start += len(keyBlock)
			}
		}
	}
	if len(certBlock) == 0 {
		return "", fmt.Errorf("failed to find CERTIFICATE")
	}
	if len(keyBlock) == 0 {
		return "", fmt.Errorf("failed to find PRIVATE KEY")
	}

	cert, err := tls.X509KeyPair(certBlock, keyBlock)
	if err != nil {
		return "", err
	}

	cfg.Certificates = append(cfg.Certificates, cert)

	// The documentation for the tls.X509KeyPair indicates that the Leaf certificate is not
	// retained.
	crt, err := x509.ParseCertificate(certDecodedBlock)
	if err != nil {
		return "", err
	}

	return crt.Subject.String(), nil
}

// addCACertFromFile adds a root CA certificate to the configuration given a path
// to the containing file.
func addCACertFromFile(cfg *tls.Config, file string) error {
	data, err := ioutil.ReadFile(file)
	if err != nil {
		return err
	}

	certBytes, err := loadCert(data)
	if err != nil {
		return err
	}

	cert, err := x509.ParseCertificate(certBytes)
	if err != nil {
		return err
	}

	if cfg.RootCAs == nil {
		cfg.RootCAs = x509.NewCertPool()
	}

	cfg.RootCAs.AddCert(cert)

	return nil
}

func loadCert(data []byte) ([]byte, error) {
	var certBlock *pem.Block

	for certBlock == nil {
		if data == nil || len(data) == 0 {
			return nil, errors.New(".pem file must have both a CERTIFICATE and an RSA PRIVATE KEY section")
		}

		block, rest := pem.Decode(data)
		if block == nil {
			return nil, errors.New("invalid .pem file")
		}

		switch block.Type {
		case "CERTIFICATE":
			if certBlock != nil {
				return nil, errors.New("multiple CERTIFICATE sections in .pem file")
			}

			certBlock = block
		}

		data = rest
	}

	return certBlock.Bytes, nil
}

// configure the client according to the options set in the uri and in the provided ToolOptions, with ToolOptions having precedence.
func configureClient(opts options.ToolOptions) (*mongo.Client, error) {
	clientopt := mopt.Client()

	if opts.RetryWrites != nil {
		clientopt.SetRetryWrites(*opts.RetryWrites)
	}

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

	clientopt.SetConnectTimeout(time.Duration(opts.Timeout) * time.Second)
	clientopt.SetSocketTimeout(time.Duration(opts.SocketTimeout) * time.Second)
	if opts.Connection.ServerSelectionTimeout > 0 {
		clientopt.SetServerSelectionTimeout(time.Duration(opts.Connection.ServerSelectionTimeout) * time.Second)
	}
	clientopt.SetReplicaSet(opts.ReplicaSetName)

	clientopt.SetAppName(opts.AppName)
	if opts.Direct {
		clientopt.SetDirect(true)
		t := true
		clientopt.AuthenticateToAnything = &t
	}

	if opts.ReadPreference != nil {
		clientopt.SetReadPreference(opts.ReadPreference)
	}
	if opts.WriteConcern != nil {
		clientopt.SetWriteConcern(opts.WriteConcern)
	} else {
		// If no write concern was specified, default to majority
		clientopt.SetWriteConcern(writeconcern.New(writeconcern.WMajority()))
	}

	if opts.Compressors != "" && opts.Compressors != "none" {
		clientopt.SetCompressors(strings.Split(opts.Compressors, ","))
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

		tlsConfig := &tls.Config{}
		if opts.SSLAllowInvalidCert || opts.SSLAllowInvalidHost {
			tlsConfig.InsecureSkipVerify = true
		}
		if opts.SSLPEMKeyFile != "" {
			_, err := addClientCertFromFile(tlsConfig, opts.SSLPEMKeyFile, opts.SSLPEMKeyPassword)
			if err != nil {
				return nil, fmt.Errorf("error configuring client, can't load client certificate: %v", err)
			}
		}
		if opts.SSLCAFile != "" {
			if err := addCACertFromFile(tlsConfig, opts.SSLCAFile); err != nil {
				return nil, fmt.Errorf("error configuring client, can't load CA file: %v", err)
			}
		}
		clientopt.SetTLSConfig(tlsConfig)
	}

	return mongo.NewClient(uriOpts, clientopt)
}

// FilterError determines whether an error needs to be propagated back to the user or can be continued through. If an
// error cannot be ignored, a non-nil error is returned. If an error can be continued through, it is logged and nil is
// returned.
func FilterError(stopOnError bool, err error) error {
	if err == nil || err.Error() == ErrUnacknowledgedWrite {
		return nil
	}

	if !stopOnError && CanIgnoreError(err) {
		// Just log the error but don't propagate it.
		if bwe, ok := err.(mongo.BulkWriteException); ok {
			for _, be := range bwe.WriteErrors {
				log.Logvf(log.Always, continueThroughErrorFormat, be.Message)
			}
		} else {
			log.Logvf(log.Always, continueThroughErrorFormat, err)
		}
		return nil
	}
	// Propagate this error, since it's either a fatal error or the user has turned on --stopOnError
	return err
}

// Returns whether the tools can continue when encountering the given error.
// Currently, only DuplicateKeyErrors are ignorable.
func CanIgnoreError(err error) bool {
	if err == nil {
		return true
	}

	switch mongoErr := err.(type) {
	case mongo.WriteError:
		_, ok := ignorableWriteErrorCodes[mongoErr.Code]
		return ok
	case mongo.BulkWriteException:
		for _, writeErr := range mongoErr.WriteErrors {
			if _, ok := ignorableWriteErrorCodes[writeErr.Code]; !ok {
				return false
			}
		}

		if mongoErr.WriteConcernError != nil {
			log.Logvf(log.Always, "write concern error when inserting documents: %v", mongoErr.WriteConcernError)
			return false
		}
		return true
	case mongo.CommandError:
		_, ok := ignorableWriteErrorCodes[int(mongoErr.Code)]
		return ok
	}

	return false
}

// IsMMAPV1 returns whether the storage engine is MMAPV1. Also returns false
// if the storage engine type cannot be determined for some reason.
func IsMMAPV1(database *mongo.Database, collectionName string) (bool, error) {
	// mmapv1 does not announce itself like other storage engines. Instead,
	// we check for the key 'numExtents', which only occurs on MMAPV1.
	const numExtents = "numExtents"

	var collStats map[string]interface{}

	singleRes := database.RunCommand(context.Background(), bson.M{"collStats": collectionName})

	if err := singleRes.Err(); err != nil {
		return false, err
	}

	if err := singleRes.Decode(&collStats); err != nil {
		return false, err
	}

	_, ok := collStats[numExtents]
	return ok, nil
}
