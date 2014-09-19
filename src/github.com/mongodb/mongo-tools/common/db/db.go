// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db/command"
	"github.com/mongodb/mongo-tools/common/options"
	"gopkg.in/mgo.v2"
	"sync"
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
	// copy the provider's master session, for connection pooling
	return self.masterSession.Copy(), nil
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
