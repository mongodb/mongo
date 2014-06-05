// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db/ssl"
	"github.com/shelman/mongo-tools-proto/common/options"
	"labix.org/v2/mgo"
	"sync"
)

const (
	// authentication types to be passed to the driver
	AUTH_STANDARD = "MONGODB-CR"
	AUTH_SSL      = "MONGODB-X509"
)

// Used to manage database sessions
type SessionProvider struct {

	// For connecting to the database
	connector DBConnector

	// used to avoid a race condition around creating the master session
	masterSessionLock sync.Mutex

	// the master session to use for connection pooling
	masterSession *mgo.Session
}

// Returns a session connected to the database server for which the
// session provider is configured.  Initializes a master session if necessary,
// in order to do connection pooling.
func (self *SessionProvider) GetSession() (*mgo.Session, error) {

	// initialize the master session, if necessary
	if self.masterSession == nil {

		// lock to avoid a race condition
		self.masterSessionLock.Lock()
		defer self.masterSessionLock.Unlock()

		// check again, in case another goroutine initialized the session in
		// between the above two checks
		if self.masterSession == nil {
			var err error
			self.masterSession, err = self.connector.GetNewSession()
			if err != nil {
				return nil, fmt.Errorf("error connecting to db server: %v", err)
			}
		}
	}

	// copy and return the master session
	return self.masterSession.Copy(), nil
}

// Initialize a session provider to connect to the database server, based on
// the options passed in.  Returns a fully initialized provider.
func InitSessionProvider(opts *options.ToolOptions) (*SessionProvider,
	error) {

	if opts == nil {
		return nil, fmt.Errorf("tool options cannot be nil")
	}

	// create the provider
	provider := &SessionProvider{}

	// create the connector for dialing the database
	provider.connector = getConnector(opts)

	// configure the connector
	provider.connector.Configure(opts)

	return provider, nil
}

// Get the right type of connector, based on the options
func getConnector(opts *options.ToolOptions) DBConnector {

	if opts.SSL.UseSSL {
		return &ssl.SSLDBConnector{}
	}

	return &VanillaDBConnector{}
}
