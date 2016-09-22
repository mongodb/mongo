// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/password"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"

	"fmt"
	"io"
	"strings"
	"sync"
)

type (
	sessionFlag uint32
	// Used to get appropriate the DBConnector(s) based on opts
	GetConnectorFunc func(opts options.ToolOptions) DBConnector
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
	ErrCouldNotFindPrimaryPrefix    = `could not find host matching read preference { mode: "primary"`
	ErrUnableToTargetPrefix         = "unable to target"
	ErrNotMaster                    = "not master"
	ErrConnectionRefusedSuffix      = "Connection refused"
)

var (
	GetConnectorFuncs = []GetConnectorFunc{}
)

// Used to manage database sessions
type SessionProvider struct {

	// For connecting to the database
	connector DBConnector

	// used to avoid a race condition around creating the master session
	masterSessionLock sync.Mutex

	// the master session to use for connection pooling
	masterSession *mgo.Session

	// flags for generating the master session
	bypassDocumentValidation bool
	flags                    sessionFlag
	readPreference           mgo.Mode
	tags                     bson.D
}

// ApplyOpsResponse represents the response from an 'applyOps' command.
type ApplyOpsResponse struct {
	Ok     bool   `bson:"ok"`
	ErrMsg string `bson:"errmsg"`
}

// Oplog represents a MongoDB oplog document.
type Oplog struct {
	Timestamp bson.MongoTimestamp `bson:"ts"`
	HistoryID int64               `bson:"h"`
	Version   int                 `bson:"v"`
	Operation string              `bson:"op"`
	Namespace string              `bson:"ns"`
	Object    bson.D              `bson:"o"`
	Query     bson.D              `bson:"o2"`
}

// Returns a session connected to the database server for which the
// session provider is configured.
func (self *SessionProvider) GetSession() (*mgo.Session, error) {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	// The master session is initialized
	if self.masterSession != nil {
		return self.masterSession.Copy(), nil
	}

	// initialize the provider's master session
	var err error
	self.masterSession, err = self.connector.GetNewSession()
	if err != nil {
		return nil, fmt.Errorf("error connecting to db server: %v", err)
	}

	// update masterSession based on flags
	self.refresh()

	// copy the provider's master session, for connection pooling
	return self.masterSession.Copy(), nil
}

// Close closes the master session in the connection pool
func (self *SessionProvider) Close() {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()
	self.masterSession.Close()
}

// refresh is a helper for modifying the session based on the
// session provider flags passed in with SetFlags.
// This helper assumes a lock is already taken.
func (self *SessionProvider) refresh() {
	// handle bypassDocumentValidation
	self.masterSession.SetBypassValidation(self.bypassDocumentValidation)

	// handle readPreference
	self.masterSession.SetMode(self.readPreference, true)

	// disable timeouts
	if (self.flags & DisableSocketTimeout) > 0 {
		self.masterSession.SetSocketTimeout(0)
	}
	if self.tags != nil {
		self.masterSession.SelectServers(self.tags)
	}
}

// SetFlags allows certain modifications to the masterSession after initial creation.
func (self *SessionProvider) SetFlags(flagBits sessionFlag) {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	self.flags = flagBits

	// make sure we update the master session if one already exists
	if self.masterSession != nil {
		self.refresh()
	}
}

// SetReadPreference sets the read preference mode in the SessionProvider
// and eventually in the masterSession
func (self *SessionProvider) SetReadPreference(pref mgo.Mode) {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	self.readPreference = pref

	if self.masterSession != nil {
		self.refresh()
	}
}

// SetBypassDocumentValidation sets whether to bypass document validation in the SessionProvider
// and eventually in the masterSession
func (self *SessionProvider) SetBypassDocumentValidation(bypassDocumentValidation bool) {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	self.bypassDocumentValidation = bypassDocumentValidation

	if self.masterSession != nil {
		self.refresh()
	}
}

// SetTags sets the server selection tags in the SessionProvider
// and eventually in the masterSession
func (self *SessionProvider) SetTags(tags bson.D) {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	self.tags = tags

	if self.masterSession != nil {
		self.refresh()
	}
}

// NewSessionProvider constructs a session provider but does not attempt to
// create the initial session.
func NewSessionProvider(opts options.ToolOptions) (*SessionProvider, error) {
	// create the provider
	provider := &SessionProvider{
		readPreference:           mgo.Primary,
		bypassDocumentValidation: false,
	}

	// finalize auth options, filling in missing passwords
	if opts.Auth.ShouldAskForPassword() {
		opts.Auth.Password = password.Prompt()
	}

	// create the connector for dialing the database
	provider.connector = getConnector(opts)

	// configure the connector
	err := provider.connector.Configure(opts)
	if err != nil {
		return nil, fmt.Errorf("error configuring the connector: %v", err)
	}
	return provider, nil
}

// IsConnectionError returns a boolean indicating if a given error is due to
// an error in an underlying DB connection (as opposed to some other write
// failure such as a duplicate key error)
func IsConnectionError(err error) bool {
	if err == nil {
		return false
	}
	if err.Error() == ErrNoReachableServers ||
		err.Error() == io.EOF.Error() ||
		strings.HasPrefix(err.Error(), ErrReplTimeoutPrefix) ||
		strings.HasPrefix(err.Error(), ErrCouldNotContactPrimaryPrefix) ||
		strings.HasPrefix(err.Error(), ErrCouldNotFindPrimaryPrefix) ||
		strings.HasPrefix(err.Error(), ErrUnableToTargetPrefix) ||
		err.Error() == ErrNotMaster ||
		strings.HasSuffix(err.Error(), ErrConnectionRefusedSuffix) {
		return true
	}
	return false
}

// Get the right type of connector, based on the options
func getConnector(opts options.ToolOptions) DBConnector {
	for _, getConnectorFunc := range GetConnectorFuncs {
		if connector := getConnectorFunc(opts); connector != nil {
			return connector
		}
	}
	return &VanillaDBConnector{}
}
