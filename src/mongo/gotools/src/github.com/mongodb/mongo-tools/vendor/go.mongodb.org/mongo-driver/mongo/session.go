// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"
	"errors"
	"time"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/operation"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
)

// ErrWrongClient is returned when a user attempts to pass in a session created by a different client than
// the method call is using.
var ErrWrongClient = errors.New("session was not created by this client")

var withTransactionTimeout = 120 * time.Second

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
	WithTransaction(ctx context.Context, fn func(sessCtx SessionContext) (interface{}, error), opts ...*options.TransactionOptions) (interface{}, error)
	StartTransaction(...*options.TransactionOptions) error
	AbortTransaction(context.Context) error
	CommitTransaction(context.Context) error
	ClusterTime() bson.Raw
	AdvanceClusterTime(bson.Raw) error
	OperationTime() *primitive.Timestamp
	AdvanceOperationTime(*primitive.Timestamp) error
	Client() *Client
	session()
}

// sessionImpl represents a set of sequential operations executed by an application that are related in some way.
type sessionImpl struct {
	clientSession       *session.Client
	client              *Client
	topo                *topology.Topology
	didCommitAfterStart bool // true if commit was called after start with no other operations
}

// EndSession ends the session.
func (s *sessionImpl) EndSession(ctx context.Context) {
	if s.clientSession.TransactionInProgress() {
		// ignore all errors aborting during an end session
		_ = s.AbortTransaction(ctx)
	}
	s.clientSession.EndSession()
}

// WithTransaction creates a transaction on this session and runs the given callback, retrying for
// TransientTransactionError and UnknownTransactionCommitResult errors. The only way to provide a
// session to a CRUD method is to invoke that CRUD method with the mongo.SessionContext within the
// callback. The mongo.SessionContext can be used as a regular context, so methods like
// context.WithDeadline and context.WithTimeout are supported.
//
// If the context.Context already has a mongo.Session attached, that mongo.Session will be replaced
// with the one provided.
//
// The callback may be run multiple times due to retry attempts. Non-retryable and timed out errors
// are returned from this function.
func (s *sessionImpl) WithTransaction(ctx context.Context, fn func(sessCtx SessionContext) (interface{}, error), opts ...*options.TransactionOptions) (interface{}, error) {
	timeout := time.NewTimer(withTransactionTimeout)
	defer timeout.Stop()
	var err error
	for {
		err = s.StartTransaction(opts...)
		if err != nil {
			return nil, err
		}

		res, err := fn(contextWithSession(ctx, s))
		if err != nil {
			if s.clientSession.TransactionRunning() {
				_ = s.AbortTransaction(ctx)
			}

			select {
			case <-timeout.C:
				return nil, err
			default:
			}

			if cerr, ok := err.(CommandError); ok {
				if cerr.HasErrorLabel(driver.TransientTransactionError) {
					continue
				}
			}
			return res, err
		}

		err = s.clientSession.CheckAbortTransaction()
		if err != nil {
			return res, nil
		}

	CommitLoop:
		for {
			err = s.CommitTransaction(ctx)
			if err == nil {
				return res, nil
			}

			select {
			case <-timeout.C:
				return res, err
			default:
			}

			if cerr, ok := err.(CommandError); ok {
				if cerr.HasErrorLabel(driver.UnknownTransactionCommitResult) && !cerr.IsMaxTimeMSExpiredError() {
					continue
				}
				if cerr.HasErrorLabel(driver.TransientTransactionError) {
					break CommitLoop
				}
			}
			return res, err
		}
	}
}

// StartTransaction starts a transaction for this session.
func (s *sessionImpl) StartTransaction(opts ...*options.TransactionOptions) error {
	err := s.clientSession.CheckStartTransaction()
	if err != nil {
		return err
	}

	s.didCommitAfterStart = false

	topts := options.MergeTransactionOptions(opts...)
	coreOpts := &session.TransactionOptions{
		ReadConcern:    topts.ReadConcern,
		ReadPreference: topts.ReadPreference,
		WriteConcern:   topts.WriteConcern,
		MaxCommitTime:  topts.MaxCommitTime,
	}

	return s.clientSession.StartTransaction(coreOpts)
}

// AbortTransaction aborts the session's transaction, returning any errors and error codes
func (s *sessionImpl) AbortTransaction(ctx context.Context) error {
	err := s.clientSession.CheckAbortTransaction()
	if err != nil {
		return err
	}

	// Do not run the abort command if the transaction is in starting state
	if s.clientSession.TransactionStarting() || s.didCommitAfterStart {
		return s.clientSession.AbortTransaction()
	}

	selector := makePinnedSelector(s.clientSession, description.WriteSelector())

	s.clientSession.Aborting = true
	_ = operation.NewAbortTransaction().Session(s.clientSession).ClusterClock(s.client.clock).Database("admin").
		Deployment(s.topo).WriteConcern(s.clientSession.CurrentWc).ServerSelector(selector).
		Retry(driver.RetryOncePerCommand).CommandMonitor(s.client.monitor).RecoveryToken(bsoncore.Document(s.clientSession.RecoveryToken)).Execute(ctx)

	s.clientSession.Aborting = false
	_ = s.clientSession.AbortTransaction()

	return nil
}

// CommitTransaction commits the sesson's transaction.
func (s *sessionImpl) CommitTransaction(ctx context.Context) error {
	err := s.clientSession.CheckCommitTransaction()
	if err != nil {
		return err
	}

	// Do not run the commit command if the transaction is in started state
	if s.clientSession.TransactionStarting() || s.didCommitAfterStart {
		s.didCommitAfterStart = true
		return s.clientSession.CommitTransaction()
	}

	if s.clientSession.TransactionCommitted() {
		s.clientSession.RetryingCommit = true
	}

	selector := makePinnedSelector(s.clientSession, description.WriteSelector())

	s.clientSession.Committing = true
	op := operation.NewCommitTransaction().
		Session(s.clientSession).ClusterClock(s.client.clock).Database("admin").Deployment(s.topo).
		WriteConcern(s.clientSession.CurrentWc).ServerSelector(selector).Retry(driver.RetryOncePerCommand).
		CommandMonitor(s.client.monitor).RecoveryToken(bsoncore.Document(s.clientSession.RecoveryToken))
	if s.clientSession.CurrentMct != nil {
		op.MaxTimeMS(int64(*s.clientSession.CurrentMct / time.Millisecond))
	}

	err = op.Execute(ctx)
	s.clientSession.Committing = false
	commitErr := s.clientSession.CommitTransaction()

	// We set the write concern to majority for subsequent calls to CommitTransaction.
	s.clientSession.UpdateCommitTransactionWriteConcern()

	if err != nil {
		return replaceErrors(err)
	}
	return commitErr
}

func (s *sessionImpl) ClusterTime() bson.Raw {
	return s.clientSession.ClusterTime
}

func (s *sessionImpl) AdvanceClusterTime(d bson.Raw) error {
	return s.clientSession.AdvanceClusterTime(d)
}

func (s *sessionImpl) OperationTime() *primitive.Timestamp {
	return s.clientSession.OperationTime
}

func (s *sessionImpl) AdvanceOperationTime(ts *primitive.Timestamp) error {
	return s.clientSession.AdvanceOperationTime(ts)
}

func (s *sessionImpl) Client() *Client {
	return s.client
}

func (*sessionImpl) session() {
}

// sessionFromContext checks for a sessionImpl in the argued context and returns the session if it
// exists
func sessionFromContext(ctx context.Context) *session.Client {
	s := ctx.Value(sessionKey{})
	if ses, ok := s.(*sessionImpl); ses != nil && ok {
		return ses.clientSession
	}

	return nil
}

func contextWithSession(ctx context.Context, sess Session) SessionContext {
	return &sessionContext{
		Context: context.WithValue(ctx, sessionKey{}, sess),
		Session: sess,
	}
}
