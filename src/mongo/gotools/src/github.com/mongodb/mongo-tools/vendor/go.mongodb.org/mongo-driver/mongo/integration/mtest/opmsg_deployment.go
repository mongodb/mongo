// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mtest

import (
	"context"

	"github.com/pkg/errors"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/address"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/wiremessage"
)

const (
	serverAddress                = address.Address("localhost:27017")
	maxDocumentSize       uint32 = 16777216
	maxMessageSize        uint32 = 48000000
	maxBatchCount         uint32 = 100000
	sessionTimeoutMinutes uint32 = 30
	maxWireVersion        int32  = 8
)

// connection implements the driver.Connection interface and responds to wire messages with pre-configured responses.
type connection struct {
	responses []bson.D // responses to send when ReadWireMessage is called
}

var _ driver.Connection = &connection{}

// WriteWireMessage is a no-op operation.
func (c *connection) WriteWireMessage(_ context.Context, wm []byte) error {
	return nil
}

// ReadWireMessage returns the next response in the connection's list of responses.
func (c *connection) ReadWireMessage(_ context.Context, dst []byte) ([]byte, error) {
	if len(c.responses) == 0 {
		return dst, errors.New("no responses remaining")
	}
	nextRes := c.responses[0]
	c.responses = c.responses[1:]

	var wmindex int32
	wmindex, dst = wiremessage.AppendHeaderStart(dst, wiremessage.NextRequestID(), 0, wiremessage.OpMsg)
	dst = wiremessage.AppendMsgFlags(dst, 0)
	dst = wiremessage.AppendMsgSectionType(dst, wiremessage.SingleDocument)
	resBytes, _ := bson.Marshal(nextRes)
	dst = append(dst, resBytes...)
	dst = bsoncore.UpdateLength(dst, wmindex, int32(len(dst[wmindex:])))
	return dst, nil
}

// Description returns a fixed server description for the connection.
func (c *connection) Description() description.Server {
	return description.Server{
		CanonicalAddr:         serverAddress,
		MaxDocumentSize:       maxDocumentSize,
		MaxMessageSize:        maxMessageSize,
		MaxBatchCount:         maxBatchCount,
		SessionTimeoutMinutes: sessionTimeoutMinutes,
		Kind:                  description.RSPrimary,
		WireVersion: &description.VersionRange{
			Max: maxWireVersion,
		},
	}
}

// Close is a no-op operation.
func (*connection) Close() error {
	return nil
}

// ID returns a fixed identifier for the connection.
func (*connection) ID() string {
	return "<mock_connection>"
}

// Address returns a fixed address for the connection.
func (*connection) Address() address.Address {
	return serverAddress
}

// mockDeployment wraps a connection and implements the driver.Deployment interface.
type mockDeployment struct {
	conn    *connection
	updates chan description.Topology
}

var _ driver.Deployment = &mockDeployment{}
var _ driver.Server = &mockDeployment{}
var _ driver.Connector = &mockDeployment{}
var _ driver.Disconnector = &mockDeployment{}
var _ driver.Subscriber = &mockDeployment{}

// SelectServer implements the Deployment interface. This method does not use the
// description.SelectedServer provided and instead returns itself. The Connections returned from the
// Connection method have a no-op Close method.
func (md *mockDeployment) SelectServer(context.Context, description.ServerSelector) (driver.Server, error) {
	return md, nil
}

// SupportsRetry implements the Deployment interface. It always returns true to allow for testing
// retryability.
func (md *mockDeployment) SupportsRetryWrites() bool {
	return true
}

// Kind implements the Deployment interface. It always returns description.Single.
func (md *mockDeployment) Kind() description.TopologyKind {
	return description.Single
}

// Connection implements the driver.Server interface.
func (md *mockDeployment) Connection(context.Context) (driver.Connection, error) {
	return md.conn, nil
}

// Connect is a no-op method which implements the driver.Connector interface.
func (md *mockDeployment) Connect() error {
	return nil
}

// Disconnect is a no-op method which implements the driver.Disconnector interface {
func (md *mockDeployment) Disconnect(context.Context) error {
	close(md.updates)
	return nil
}

// Subscribe returns a subscription from which new topology descriptions can be retrieved.
// Subscribe implements the driver.Subscriber interface.
func (md *mockDeployment) Subscribe() (*driver.Subscription, error) {
	if md.updates == nil {
		md.updates = make(chan description.Topology, 1)
		md.updates <- description.Topology{
			SessionTimeoutMinutes: sessionTimeoutMinutes,
		}
	}

	return &driver.Subscription{
		Updates: md.updates,
	}, nil
}

// Unsubscribe is a no-op method which implements the driver.Subscriber interface.
func (md *mockDeployment) Unsubscribe(*driver.Subscription) error {
	return nil
}

// addResponses adds responses to this mock deployment.
func (md *mockDeployment) addResponses(responses ...bson.D) {
	md.conn.responses = append(md.conn.responses, responses...)
}

// clearResponses clears all remaining responses in this mock deployment.
func (md *mockDeployment) clearResponses() {
	md.conn.responses = md.conn.responses[:0]
}

// newMockDeployment returns a mock driver.Deployment that responds with OP_MSG wire messages.
func newMockDeployment(responses ...bson.D) *mockDeployment {
	return &mockDeployment{
		conn: &connection{
			responses: responses,
		},
	}
}
