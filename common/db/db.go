// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/db/command"
	"github.com/mongodb/mongo-tools/common/options"
	"gopkg.in/mgo.v2"
	"io"
	"sync"
)

const MaxBSONSize = 16 * 1024 * 1024

type sessionFlag uint32

const (
	None      sessionFlag = 0
	Monotonic sessionFlag = 1 << iota
)

var (
	ErrLostConnection     = errors.New("lost connection to server")
	ErrNoReachableServers = errors.New("no reachable servers")
	ErrNsNotFound         = errors.New("ns not found")
)

type GetConnectorFunc func(opts options.ToolOptions) DBConnector

var GetConnectorFuncs []GetConnectorFunc

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

func (self *SessionProvider) RunCommand(dbToUse string,
	cmd command.Command) error {

	session, err := self.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	return session.DB(dbToUse).Run(cmd.AsRunnable(), cmd)
}

// Returns a session connected to the database server for which the
// session provider is configured.
func (self *SessionProvider) GetSession() (*mgo.Session, error) {
	//The master session is initialized
	if self.masterSession != nil {
		return self.masterSession.Copy(), nil
	}

	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	if self.masterSession != nil {
		return self.masterSession.Copy(), nil
	}

	// initialize the provider's master session
	var err error
	self.masterSession, err = self.connector.GetNewSession()
	if err != nil {
		return nil, fmt.Errorf("error connecting to db server: %v", err)
	}
	// handle session flags
	if (self.flags & Monotonic) > 0 {
		self.masterSession.SetMode(mgo.Monotonic, true)
	}
	// copy the provider's master session, for connection pooling
	return self.masterSession.Copy(), nil
}

// SetFlags allows certain modifications to the masterSession after
// initial creation.
func (self *SessionProvider) SetFlags(flagBits sessionFlag) {
	self.masterSessionLock.Lock()
	defer self.masterSessionLock.Unlock()

	// make sure this is not done after initial creation
	if self.masterSession != nil {
		panic("cannot set session provider flags after calling GetSession()")
	}

	self.flags = flagBits
}

//NewSessionProvider constructs a session provider but does not attempt to
//create the initial session.
func NewSessionProvider(opts options.ToolOptions) *SessionProvider {
	// create the provider
	provider := &SessionProvider{}

	// create the connector for dialing the database
	provider.connector = getConnector(opts)

	// configure the connector
	provider.connector.Configure(opts)

	return provider

}

// IsConnectionError returns a boolean indicating if a given error is due to
// an error in an underlying DB connection (as opposed to some other write
// failure such as a duplicate key error)
func IsConnectionError(err error) bool {
	if err == nil {
		return false
	}
	if err.Error() == ErrNoReachableServers.Error() {
		return true
	}
	if err.Error() == io.EOF.Error() {
		return true
	}
	return false
}

// Initialize a session provider to connect to the database server, based on
// the options passed in.  Connects to the db and returns a fully initialized
// provider.
func InitSessionProvider(opts options.ToolOptions) (*SessionProvider,
	error) {

	// create the provider
	provider := &SessionProvider{}

	// create the connector for dialing the database
	provider.connector = getConnector(opts)

	// configure the connector
	err := provider.connector.Configure(opts)
	if err != nil {
		return nil, fmt.Errorf("error configuring the connector: %v", err)
	}

	// initialize the provider's master session
	provider.masterSession, err = provider.connector.GetNewSession()
	if err != nil {
		return nil, fmt.Errorf("error connecting to db server: %v", err)
	}

	return provider, nil
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
