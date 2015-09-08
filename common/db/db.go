// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/password"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"sync"
	"time"
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
	MaxBSONSize    = 16 * 1024 * 1024     // 16MB - maximum BSON document size
	MaxMessageSize = 2 * 16 * 1024 * 1024 // 32MB - maximum message size in wire protocol
)

// Default port for integration tests
const (
	DefaultTestPort = "33333"
)

var (
	ErrLostConnection     = errors.New("lost connection to server")
	ErrNoReachableServers = errors.New("no reachable servers")
	ErrNsNotFound         = errors.New("ns not found")
	ErrSlaveTimeout       = errors.New("timed out waiting for slaves")
	DefaultDialTimeout    = time.Second * 3
	GetConnectorFuncs     = []GetConnectorFunc{}
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
	flags sessionFlag
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
	self.refreshFlags()

	// copy the provider's master session, for connection pooling
	return self.masterSession.Copy(), nil
}

// refreshFlags is a helper for modifying the session based on the
// session provider flags passed in with SetFlags.
// This helper assumes a lock is already taken.
func (self *SessionProvider) refreshFlags() {
	// handle slaveOK
	if (self.flags & Monotonic) > 0 {
		self.masterSession.SetMode(mgo.Monotonic, true)
	} else {
		self.masterSession.SetMode(mgo.Strong, true)
	}
	// disable timeouts
	if (self.flags & DisableSocketTimeout) > 0 {
		self.masterSession.SetSocketTimeout(0)
	}
}

// SetFlags allows certain modifications to the masterSession after initial creation.
func (self *SessionProvider) SetFlags(flagBits sessionFlag) {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	self.flags = flagBits

	// make sure we update the master session if one already exists
	if self.masterSession != nil {
		self.refreshFlags()
	}
}

// NewSessionProvider constructs a session provider but does not attempt to
// create the initial session.
func NewSessionProvider(opts options.ToolOptions) (*SessionProvider, error) {
	// create the provider
	provider := &SessionProvider{}

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
	if err.Error() == ErrNoReachableServers.Error() ||
		err.Error() == ErrSlaveTimeout.Error() ||
		err.Error() == io.EOF.Error() {
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
