// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mtest

import (
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/options"
)

// TopologyKind describes the topology that a test is run on.
type TopologyKind string

// These constants specify valid values for TopologyKind
const (
	ReplicaSet TopologyKind = "replicaset"
	Sharded                 = "sharded"
	Single                  = "single"
)

// ClientType specifies the type of Client that should be created for a test.
type ClientType int

// These constants specify valid values for ClientType
const (
	// Default specifies a client to the connection string in the MONGODB_URI env variable with command monitoring
	// enabled.
	Default ClientType = iota
	// Pinned specifies a client that is pinned to a single mongos in a sharded cluster.
	Pinned
	// Mock specifies a client that communicates with a mock deployment.
	Mock
)

// RunOnBlock describes a constraint for a test.
type RunOnBlock struct {
	MinServerVersion string         `bson:"minServerVersion"`
	MaxServerVersion string         `bson:"maxServerVersion"`
	Topology         []TopologyKind `bson:"topology"`
}

// optionFunc is a function type that configures a T instance.
type optionFunc func(*T)

// Options is the type used to configure a new T instance.
type Options struct {
	optFuncs []optionFunc
}

// NewOptions creates an empty Options instance.
func NewOptions() *Options {
	return &Options{}
}

// CollectionCreateOptions sets the options to pass to the create command when creating a collection for a test.
// For example, if opts = {"capped": "true"}, the create command sent to the server will be
// {create: <collectionName>, foo: bar}.
func (op *Options) CollectionCreateOptions(opts bson.D) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.collCreateOpts = opts
	})
	return op
}

// CollectionOptions sets the options to use when creating a collection for a test.
func (op *Options) CollectionOptions(opts *options.CollectionOptions) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.collOpts = opts
	})
	return op
}

// ClientOptions sets the options to use when creating a client for a test.
func (op *Options) ClientOptions(opts *options.ClientOptions) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.clientOpts = opts
	})
	return op
}

// CreateClient specifies whether or not a client and collection should be created for a test. This should be set to
// false when running a test that only runs other tests.
func (op *Options) CreateClient(create bool) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.createClient = &create
	})
	return op
}

// ShareClient specifies whether or not a test should pass its client down to sub-tests. This should be set when calling
// New() if the inheriting behavior is desired. This option must not be used if the test accesses command monitoring
// events.
func (op *Options) ShareClient(share bool) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.shareClient = &share
	})
	return op
}

// CollectionName specifies the name for the collection for the test.
func (op *Options) CollectionName(collName string) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.collName = collName
	})
	return op
}

// DatabaseName specifies the name of the database for the test.
func (op *Options) DatabaseName(dbName string) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.dbName = dbName
	})
	return op
}

// ClientType specifies the type of client that should be created for a test. This option will be propagated to all
// sub-tests.
func (op *Options) ClientType(ct ClientType) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.clientType = ct
	})
	return op
}

// MockResponses specifies the responses returned by a mock deployment. This should only be used if the current test
// is being run with MockDeployment(true). Responses can also be added after a sub-test has already been created.
func (op *Options) MockResponses(responses ...bson.D) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.mockResponses = responses
	})
	return op
}

// RunOn specifies run-on blocks used to determine if a test should run. If a test's environment meets at least one of the
// given constraints, it will be run. Otherwise, it will be skipped.
func (op *Options) RunOn(blocks ...RunOnBlock) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.runOn = append(t.runOn, blocks...)
	})
	return op
}

// MinServerVersion specifies the minimum server version for the test.
func (op *Options) MinServerVersion(version string) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.minServerVersion = version
	})
	return op
}

// MaxServerVersion specifies the maximum server version for the test.
func (op *Options) MaxServerVersion(version string) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.maxServerVersion = version
	})
	return op
}

// Topologies specifies a list of topologies that the test can run on.
func (op *Options) Topologies(topos ...TopologyKind) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.validTopologies = topos
	})
	return op
}

// Auth specifies whether or not auth should be enabled for this test to run. By default, a test will run regardless
// of whether or not auth is enabled.
func (op *Options) Auth(auth bool) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.auth = &auth
	})
	return op
}

// Enterprise specifies whether or not this test should only be run on enterprise server variants. Defaults to false.
func (op *Options) Enterprise(ent bool) *Options {
	op.optFuncs = append(op.optFuncs, func(t *T) {
		t.enterprise = &ent
	})
	return op
}
