// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
)

// ErrWrongClient is returned when a user attempts to pass in a session created by a different client than
// the method call is using.
var ErrWrongClient = errors.New("session was not created by this client")

// SessionContext is a hybrid interface. It combines a context.Context with
// a mongo.Session. This type can be used as a regular context.Context or
// Session type. It is not goroutine safe and should not be used in multiple goroutines concurrently.
type SessionContext interface {
	context.Context
	Session
}

type sessionContext struct {
	context.Context
	Session
}

type sessionKey struct {
}

// Session is the interface that represents a sequential set of operations executed.
// Instances of this interface can be used to use transactions against the server
// and to enable causally consistent behavior for applications.
type Session interface {
	EndSession(context.Context)
	StartTransaction(...*options.TransactionOptions) error
	AbortTransaction(context.Context) error
	CommitTransaction(context.Context) error
	ClusterTime() bson.Raw
	AdvanceClusterTime(bson.Raw) error
	OperationTime() *primitive.Timestamp
	AdvanceOperationTime(*primitive.Timestamp) error
	session()
}

// sessionImpl represents a set of sequential operations executed by an application that are related in some way.
type sessionImpl struct {
	*session.Client
	topo                *topology.Topology
	didCommitAfterStart bool // true if commit was called after start with no other operations
}

// EndSession ends the session.
func (s *sessionImpl) EndSession(ctx context.Context) {
	if s.TransactionInProgress() {
		// ignore all errors aborting during an end session
		_ = s.AbortTransaction(ctx)
	}
	s.Client.EndSession()
}

// StartTransaction starts a transaction for this session.
func (s *sessionImpl) StartTransaction(opts ...*options.TransactionOptions) error {
	err := s.CheckStartTransaction()
	if err != nil {
		return err
	}

	s.didCommitAfterStart = false

	topts := options.MergeTransactionOptions(opts...)
	coreOpts := &session.TransactionOptions{
		ReadConcern:    topts.ReadConcern,
		ReadPreference: topts.ReadPreference,
		WriteConcern:   topts.WriteConcern,
	}

	return s.Client.StartTransaction(coreOpts)
}

// AbortTransaction aborts the session's transaction, returning any errors and error codes
func (s *sessionImpl) AbortTransaction(ctx context.Context) error {
	err := s.CheckAbortTransaction()
	if err != nil {
		return err
	}

	cmd := command.AbortTransaction{
		Session: s.Client,
	}

	s.Aborting = true
	_, err = driverlegacy.AbortTransaction(ctx, cmd, s.topo, description.WriteSelector())

	_ = s.Client.AbortTransaction()
	return replaceErrors(err)
}

// CommitTransaction commits the sesson's transaction.
func (s *sessionImpl) CommitTransaction(ctx context.Context) error {
	err := s.CheckCommitTransaction()
	if err != nil {
		return err
	}

	// Do not run the commit command if the transaction is in started state
	if s.TransactionStarting() || s.didCommitAfterStart {
		s.didCommitAfterStart = true
		return s.Client.CommitTransaction()
	}

	if s.Client.TransactionCommitted() {
		s.RetryingCommit = true
	}

	cmd := command.CommitTransaction{
		Session: s.Client,
	}

	// Hack to ensure that session stays in committed state
	if s.TransactionCommitted() {
		s.Committing = true
		defer func() {
			s.Committing = false
		}()
	}
	_, err = driverlegacy.CommitTransaction(ctx, cmd, s.topo, description.WriteSelector())
	if err == nil {
		return s.Client.CommitTransaction()
	}
	return replaceErrors(err)
}

func (s *sessionImpl) ClusterTime() bson.Raw {
	return s.Client.ClusterTime
}

func (s *sessionImpl) AdvanceClusterTime(d bson.Raw) error {
	return s.Client.AdvanceClusterTime(d)
}

func (s *sessionImpl) OperationTime() *primitive.Timestamp {
	return s.Client.OperationTime
}

func (s *sessionImpl) AdvanceOperationTime(ts *primitive.Timestamp) error {
	return s.Client.AdvanceOperationTime(ts)
}

func (*sessionImpl) session() {
}

// sessionFromContext checks for a sessionImpl in the argued context and returns the session if it
// exists
func sessionFromContext(ctx context.Context) *session.Client {
	s := ctx.Value(sessionKey{})
	if ses, ok := s.(*sessionImpl); ses != nil && ok {
		return ses.Client
	}

	return nil
}

func contextWithSession(ctx context.Context, sess Session) SessionContext {
	return &sessionContext{
		Context: context.WithValue(ctx, sessionKey{}, sess),
		Session: sess,
	}
}
