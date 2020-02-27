// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mtest

import (
	"context"
	"fmt"
	"strings"
	"testing"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/event"
	"go.mongodb.org/mongo-driver/internal/testutil/assert"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
)

var (
	// Background is a no-op context.
	Background = context.Background()
	// MajorityWc is the majority write concern.
	MajorityWc = writeconcern.New(writeconcern.WMajority())
	// PrimaryRp is the primary read preference.
	PrimaryRp = readpref.Primary()
	// LocalRc is the local read concern
	LocalRc = readconcern.Local()
	// MajorityRc is the majority read concern
	MajorityRc = readconcern.Majority()
)

const (
	namespaceExistsErrCode int32 = 48
)

// FailPoint is a representation of a server fail point.
// See https://github.com/mongodb/specifications/tree/master/source/transactions/tests#server-fail-point
// for more information regarding fail points.
type FailPoint struct {
	ConfigureFailPoint string `bson:"configureFailPoint"`
	// Mode should be a string, FailPointMode, or map[string]interface{}
	Mode interface{}   `bson:"mode"`
	Data FailPointData `bson:"data"`
}

// FailPointMode is a representation of the Failpoint.Mode field.
type FailPointMode struct {
	Times int32 `bson:"times"`
	Skip  int32 `bson:"skip"`
}

// FailPointData is a representation of the FailPoint.Data field.
type FailPointData struct {
	FailCommands                  []string `bson:"failCommands,omitempty"`
	CloseConnection               bool     `bson:"closeConnection,omitempty"`
	ErrorCode                     int32    `bson:"errorCode,omitempty"`
	FailBeforeCommitExceptionCode int32    `bson:"failBeforeCommitExceptionCode,omitempty"`
	WriteConcernError             *struct {
		Code   int32  `bson:"code"`
		Name   string `bson:"codeName"`
		Errmsg string `bson:"errmsg"`
	} `bson:"writeConcernError,omitempty"`
}

// T is a wrapper around testing.T.
type T struct {
	*testing.T

	// members for only this T instance
	createClient     *bool
	runOn            []RunOnBlock
	mockDeployment   *mockDeployment // nil if the test is not being run against a mock
	mockResponses    []bson.D
	createdColls     []*mongo.Collection // collections created in this test
	dbName, collName string
	failPointNames   []string
	minServerVersion string
	maxServerVersion string
	validTopologies  []TopologyKind
	auth             *bool
	enterprise       *bool
	collCreateOpts   bson.D
	connsCheckedOut  int // net number of connections checked out during test execution

	// options copied to sub-tests
	clientType  ClientType
	clientOpts  *options.ClientOptions
	collOpts    *options.CollectionOptions
	shareClient *bool

	baseOpts *Options // used to create subtests

	// command monitoring channels
	started   []*event.CommandStartedEvent
	succeeded []*event.CommandSucceededEvent
	failed    []*event.CommandFailedEvent

	Client *mongo.Client
	DB     *mongo.Database
	Coll   *mongo.Collection
}

func newT(wrapped *testing.T, opts ...*Options) *T {
	t := &T{
		T: wrapped,
	}
	for _, opt := range opts {
		for _, optFn := range opt.optFuncs {
			optFn(t)
		}
	}

	if t.shouldSkip() {
		t.Skip("no matching environmental constraint found")
	}

	if t.collName == "" {
		t.collName = t.Name()
	}
	if t.dbName == "" {
		t.dbName = TestDb
	}
	t.collName = sanitizeCollectionName(t.dbName, t.collName)

	// create a set of base options for sub-tests
	t.baseOpts = NewOptions().ClientOptions(t.clientOpts).CollectionOptions(t.collOpts).ClientType(t.clientType)
	if t.shareClient != nil {
		t.baseOpts.ShareClient(*t.shareClient)
	}

	return t
}

// New creates a new T instance with the given options. If the current environment does not satisfy constraints
// specified in the options, the test will be skipped automatically.
func New(wrapped *testing.T, opts ...*Options) *T {
	t := newT(wrapped, opts...)

	// only create a client if it needs to be shared in sub-tests
	// otherwise, a new client will be created for each subtest
	if t.shareClient != nil && *t.shareClient {
		t.createTestClient()
	}

	return t
}

// Close cleans up any resources associated with a T. There should be one Close corresponding to every New.
func (t *T) Close() {
	if t.Client == nil {
		return
	}

	// only clear collections and fail points if the test is not running against a mock
	if t.clientType != Mock {
		t.ClearCollections()
		t.ClearFailPoints()
	}

	// always disconnect the client regardless of clientType because Client.Disconnect will work against
	// all deployments
	_ = t.Client.Disconnect(Background)
}

// Run creates a new T instance for a sub-test and runs the given callback. It also creates a new collection using the
// given name which is available to the callback through the T.Coll variable and is dropped after the callback
// returns.
func (t *T) Run(name string, callback func(*T)) {
	t.RunOpts(name, NewOptions(), callback)
}

// RunOpts creates a new T instance for a sub-test with the given options. If the current environment does not satisfy
// constraints specified in the options, the new sub-test will be skipped automatically. If the test is not skipped,
// the callback will be run with the new T instance. RunOpts creates a new collection with the given name which is
// available to the callback through the T.Coll variable and is dropped after the callback returns.
func (t *T) RunOpts(name string, opts *Options, callback func(*T)) {
	t.T.Run(name, func(wrapped *testing.T) {
		sub := newT(wrapped, t.baseOpts, opts)

		// add any mock responses for this test
		if sub.clientType == Mock && len(sub.mockResponses) > 0 {
			sub.AddMockResponses(sub.mockResponses...)
		}

		// for shareClient, inherit the client from the parent
		if sub.shareClient != nil && *sub.shareClient && sub.clientType == t.clientType {
			sub.Client = t.Client
		}
		// only create a client if not already set
		if sub.Client == nil {
			if sub.createClient == nil || *sub.createClient {
				sub.createTestClient()
			}
		}
		// create a collection for this test
		if sub.Client != nil {
			sub.createTestCollection()
		}

		// defer dropping all collections if the test is using a client
		defer func() {
			if sub.Client == nil {
				return
			}

			// store number of sessions and connections checked out here but assert that they're equal to 0 after
			// cleaning up test resources to make sure resources are always cleared
			sessions := sub.Client.NumberSessionsInProgress()
			conns := sub.connsCheckedOut

			if sub.clientType != Mock {
				sub.ClearFailPoints()
				sub.ClearCollections()
			}
			// only disconnect client if it's not being shared
			if sub.shareClient == nil || !*sub.shareClient {
				_ = sub.Client.Disconnect(Background)
			}
			assert.Equal(sub, 0, sessions, "%v sessions checked out", sessions)
			assert.Equal(sub, 0, conns, "%v connections checked out", conns)
		}()

		// clear any events that may have happened during setup and run the test
		sub.ClearEvents()
		callback(sub)
	})
}

// AddMockResponses adds responses to be returned by the mock deployment. This should only be used if T is being run
// against a mock deployment.
func (t *T) AddMockResponses(responses ...bson.D) {
	t.mockDeployment.addResponses(responses...)
}

// ClearMockResponses clears all responses in the mock deployment.
func (t *T) ClearMockResponses() {
	t.mockDeployment.clearResponses()
}

// GetStartedEvent returns the most recent CommandStartedEvent, or nil if one is not present.
// This can only be called once per event.
func (t *T) GetStartedEvent() *event.CommandStartedEvent {
	if len(t.started) == 0 {
		return nil
	}
	e := t.started[0]
	t.started = t.started[1:]
	return e
}

// GetSucceededEvent returns the most recent CommandSucceededEvent, or nil if one is not present.
// This can only be called once per event.
func (t *T) GetSucceededEvent() *event.CommandSucceededEvent {
	if len(t.succeeded) == 0 {
		return nil
	}
	e := t.succeeded[0]
	t.succeeded = t.succeeded[1:]
	return e
}

// GetFailedEvent returns the most recent CommandFailedEvent, or nil if one is not present.
// This can only be called once per event.
func (t *T) GetFailedEvent() *event.CommandFailedEvent {
	if len(t.failed) == 0 {
		return nil
	}
	e := t.failed[0]
	t.failed = t.failed[1:]
	return e
}

// GetAllStartedEvents returns a slice of all CommandStartedEvent instances for this test. This can be called multiple
// times.
func (t *T) GetAllStartedEvents() []*event.CommandStartedEvent {
	return t.started
}

// GetAllSucceededEvents returns a slice of all CommandSucceededEvent instances for this test. This can be called multiple
// times.
func (t *T) GetAllSucceededEvents() []*event.CommandSucceededEvent {
	return t.succeeded
}

// GetAllFailedEvents returns a slice of all CommandFailedEvent instances for this test. This can be called multiple
// times.
func (t *T) GetAllFailedEvents() []*event.CommandFailedEvent {
	return t.failed
}

// ClearEvents clears the existing command monitoring events.
func (t *T) ClearEvents() {
	t.started = t.started[:0]
	t.succeeded = t.succeeded[:0]
	t.failed = t.failed[:0]
}

// ResetClient resets the existing client with the given options. If opts is nil, the existing options will be used.
// If t.Coll is not-nil, it will be reset to use the new client. Should only be called if the existing client is
// not nil. This will Disconnect the existing client but will not drop existing collections. To do so, ClearCollections
// must be called before calling ResetClient.
func (t *T) ResetClient(opts *options.ClientOptions) {
	if opts != nil {
		t.clientOpts = opts
	}

	_ = t.Client.Disconnect(Background)
	t.createTestClient()
	t.DB = t.Client.Database(t.dbName)
	t.Coll = t.DB.Collection(t.collName)

	created := make([]*mongo.Collection, len(t.createdColls))
	for i, coll := range t.createdColls {
		if coll.Name() == t.collName {
			created[i] = t.Coll
			continue
		}

		created[i] = t.DB.Collection(coll.Name())
	}
	t.createdColls = created
}

// Collection is used to configure a new collection created during a test.
type Collection struct {
	Name       string
	DB         string        // defaults to mt.DB.Name() if not specified
	Client     *mongo.Client // defaults to mt.Client if not specified
	Opts       *options.CollectionOptions
	CreateOpts bson.D
}

// returns database to use for creating a new collection
func (t *T) extractDatabase(coll Collection) *mongo.Database {
	// default to t.DB unless coll overrides it
	var createNewDb bool
	dbName := t.DB.Name()
	if coll.DB != "" {
		createNewDb = true
		dbName = coll.DB
	}

	// if a client is specified, a new database must be created
	if coll.Client != nil {
		return coll.Client.Database(dbName)
	}
	// if dbName is the same as t.DB.Name(), t.DB can be used
	if !createNewDb {
		return t.DB
	}
	// a new database must be created from t.Client
	return t.Client.Database(dbName)
}

// CreateCollection creates a new collection with the given configuration. The collection will be dropped after the test
// finishes running. If createOnServer is true, the function ensures that the collection has been created server-side
// by running the create command. The create command will appear in command monitoring channels.
func (t *T) CreateCollection(coll Collection, createOnServer bool) *mongo.Collection {
	db := t.extractDatabase(coll)
	if createOnServer && t.clientType != Mock {
		cmd := bson.D{{"create", coll.Name}}
		cmd = append(cmd, coll.CreateOpts...)

		if err := db.RunCommand(Background, cmd).Err(); err != nil {
			// ignore NamespaceExists errors for idempotency

			cmdErr, ok := err.(mongo.CommandError)
			if !ok || cmdErr.Code != namespaceExistsErrCode {
				t.Fatalf("error creating collection %v on server: %v", coll.Name, err)
			}
		}
	}

	created := db.Collection(coll.Name, coll.Opts)
	t.createdColls = append(t.createdColls, created)
	return created
}

// ClearCollections drops all collections previously created by this test.
func (t *T) ClearCollections() {
	for _, coll := range t.createdColls {
		_ = coll.Drop(Background)
	}
	t.createdColls = t.createdColls[:0]
}

// SetFailPoint sets a fail point for the client associated with T. Commands to create the failpoint will appear
// in command monitoring channels. The fail point will automatically be disabled after this test has run.
func (t *T) SetFailPoint(fp FailPoint) {
	// ensure mode fields are int32
	if modeMap, ok := fp.Mode.(map[string]interface{}); ok {
		var key string
		var err error

		if times, ok := modeMap["times"]; ok {
			key = "times"
			modeMap["times"], err = t.interfaceToInt32(times)
		}
		if skip, ok := modeMap["skip"]; ok {
			key = "skip"
			modeMap["skip"], err = t.interfaceToInt32(skip)
		}

		if err != nil {
			t.Fatalf("error converting %s to int32: %v", key, err)
		}
	}

	admin := t.Client.Database("admin")
	if err := admin.RunCommand(Background, fp).Err(); err != nil {
		t.Fatalf("error creating fail point on server: %v", err)
	}
	t.failPointNames = append(t.failPointNames, fp.ConfigureFailPoint)
}

// TrackFailPoint adds the given fail point to the list of fail points to be disabled when the current test finishes.
// This function does not create a fail point on the server.
func (t *T) TrackFailPoint(fpName string) {
	t.failPointNames = append(t.failPointNames, fpName)
}

// ClearFailPoints disables all previously set failpoints for this test.
func (t *T) ClearFailPoints() {
	db := t.Client.Database("admin")
	for _, fp := range t.failPointNames {
		cmd := bson.D{
			{"configureFailPoint", fp},
			{"mode", "off"},
		}
		err := db.RunCommand(Background, cmd).Err()
		if err != nil {
			t.Fatalf("error clearing fail point %s: %v", fp, err)
		}
	}
	t.failPointNames = t.failPointNames[:0]
}

// AuthEnabled returns whether or not this test is running in an environment with auth.
func (t *T) AuthEnabled() bool {
	return testContext.authEnabled
}

// TopologyKind returns the topology kind of the environment
func (t *T) TopologyKind() TopologyKind {
	return testContext.topoKind
}

// ConnString returns the connection string used to create the client for this test.
func (t *T) ConnString() string {
	return testContext.connString.Original
}

// CloneDatabase modifies the default database for this test to match the given options.
func (t *T) CloneDatabase(opts *options.DatabaseOptions) {
	t.DB = t.Client.Database(t.dbName, opts)
}

// CloneCollection modifies the default collection for this test to match the given options.
func (t *T) CloneCollection(opts *options.CollectionOptions) {
	var err error
	t.Coll, err = t.Coll.Clone(opts)
	assert.Nil(t, err, "error cloning collection: %v", err)
}

// GlobalClient returns a client configured with read concern majority, write concern majority, and read preference
// primary. The returned client is not tied to the receiver and is valid outside the lifetime of the receiver.
func (T) GlobalClient() *mongo.Client {
	return testContext.client
}

func sanitizeCollectionName(db string, coll string) string {
	// Collections can't have "$" in their names, so we substitute it with "%".
	coll = strings.Replace(coll, "$", "%", -1)

	// Namespaces can only have 120 bytes max.
	if len(db+"."+coll) >= 120 {
		// coll len must be <= remaining
		remaining := 120 - (len(db) + 1) // +1 for "."
		coll = coll[len(coll)-remaining:]
	}
	return coll
}

func (t *T) createTestClient() {
	clientOpts := t.clientOpts
	if clientOpts == nil {
		// default opts
		clientOpts = options.Client().SetWriteConcern(MajorityWc).SetReadPreference(PrimaryRp)
	}
	// command monitor
	clientOpts.SetMonitor(&event.CommandMonitor{
		Started: func(_ context.Context, cse *event.CommandStartedEvent) {
			t.started = append(t.started, cse)
		},
		Succeeded: func(_ context.Context, cse *event.CommandSucceededEvent) {
			t.succeeded = append(t.succeeded, cse)
		},
		Failed: func(_ context.Context, cfe *event.CommandFailedEvent) {
			t.failed = append(t.failed, cfe)
		},
	})
	// only specify connection pool monitor if no deployment is given
	if clientOpts.Deployment == nil {
		previousPoolMonitor := clientOpts.PoolMonitor

		clientOpts.SetPoolMonitor(&event.PoolMonitor{
			Event: func(evt *event.PoolEvent) {
				if previousPoolMonitor != nil {
					previousPoolMonitor.Event(evt)
				}

				switch evt.Type {
				case event.GetSucceeded:
					t.connsCheckedOut++
				case event.ConnectionReturned:
					t.connsCheckedOut--
				}
			},
		})
	}

	var err error
	switch t.clientType {
	case Default:
		// only specify URI if the deployment is not set to avoid setting topology/server options along with the deployment
		if clientOpts.Deployment == nil {
			clientOpts.ApplyURI(testContext.connString.Original)
		}
		t.Client, err = mongo.NewClient(clientOpts)
	case Pinned:
		// pin to first mongos
		clientOpts.ApplyURI(testContext.connString.Original).SetHosts([]string{testContext.connString.Hosts[0]})
		t.Client, err = mongo.NewClient(clientOpts)
	case Mock:
		// clear pool monitor to avoid configuration error
		clientOpts.PoolMonitor = nil
		t.mockDeployment = newMockDeployment()
		clientOpts.Deployment = t.mockDeployment
		t.Client, err = mongo.NewClient(clientOpts)
	}
	if err != nil {
		t.Fatalf("error creating client: %v", err)
	}
	if err := t.Client.Connect(Background); err != nil {
		t.Fatalf("error connecting client: %v", err)
	}
}

func (t *T) createTestCollection() {
	t.DB = t.Client.Database(t.dbName)
	t.createdColls = t.createdColls[:0]
	t.Coll = t.CreateCollection(Collection{
		Name:       t.collName,
		CreateOpts: t.collCreateOpts,
		Opts:       t.collOpts,
	}, true)
}

// matchesServerVersion checks if the current server version is in the range [min, max]. Server versions will only be
// compared if they are non-empty.
func matchesServerVersion(min, max string) bool {
	if min != "" && compareVersions(testContext.serverVersion, min) < 0 {
		return false
	}
	return max == "" || compareVersions(testContext.serverVersion, max) <= 0
}

// matchesTopology checks if the current topology is present in topologies.
// if topologies is empty, true is returned without any additional checks.
func matchesTopology(topologies []TopologyKind) bool {
	if len(topologies) == 0 {
		return true
	}

	for _, topo := range topologies {
		if topo == testContext.topoKind {
			return true
		}
	}
	return false
}

// matchesRunOnBlock returns true if the current environmental constraints match the given RunOnBlock.
func matchesRunOnBlock(rob RunOnBlock) bool {
	if !matchesServerVersion(rob.MinServerVersion, rob.MaxServerVersion) {
		return false
	}
	return matchesTopology(rob.Topology)
}

func (t *T) shouldSkip() bool {
	// Check constraints not specified as runOn blocks
	if !matchesServerVersion(t.minServerVersion, t.maxServerVersion) {
		return true
	}
	if !matchesTopology(t.validTopologies) {
		return true
	}
	if t.auth != nil && *t.auth != testContext.authEnabled {
		return true
	}
	if t.enterprise != nil && *t.enterprise != testContext.enterpriseServer {
		return true
	}

	// Check runOn blocks
	// The test can be executed if there are no blocks or at least block matches the current test setup.
	if len(t.runOn) == 0 {
		return false
	}
	for _, runOn := range t.runOn {
		if matchesRunOnBlock(runOn) {
			return false
		}
	}
	// no matching block found
	return true
}

func (t *T) interfaceToInt32(i interface{}) (int32, error) {
	switch conv := i.(type) {
	case int:
		return int32(conv), nil
	case int32:
		return conv, nil
	case int64:
		return int32(conv), nil
	case float64:
		return int32(conv), nil
	}

	return 0, fmt.Errorf("type %T cannot be converted to int32", i)
}
