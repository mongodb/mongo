// Package db implements generic connection to MongoDB, and contains
// subpackages for specific methods of connection.
package db

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db/command"
	"github.com/shelman/mongo-tools-proto/common/db/ssl"
	"github.com/shelman/mongo-tools-proto/common/db/kerberos"
	"github.com/shelman/mongo-tools-proto/common/options"
	"labix.org/v2/mgo"
	"sync"
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

func (self *SessionProvider) RunCommand(dbToUse string,
	cmd command.Command) error {

	session := self.GetSession()
	defer session.Close()

	return session.DB(dbToUse).Run(cmd.AsRunnable(), cmd)
}

// Returns a session connected to the database server for which the
// session provider is configured.
func (self *SessionProvider) GetSession() *mgo.Session {

	// copy the provider's master session, for connection pooling
	return self.masterSession.Copy()
}

// Initialize a session provider to connect to the database server, based on
// the options passed in.  Connects to the db and returns a fully initialized
// provider.
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

	// initialize the provider's master session
	var err error
	provider.masterSession, err = provider.connector.GetNewSession()
	if err != nil {
		return nil, fmt.Errorf("error connecting to db server: %v", err)
	}

	return provider, nil
}

// Get the right type of connector, based on the options
func getConnector(opts *options.ToolOptions) DBConnector {
	if opts.Auth.Mechanism == "GSSAPI" {
		return &kerberos.KerberosDBConnector{}
	}

	if opts.SSL.UseSSL {
		return &ssl.SSLDBConnector{}
	}

	return &VanillaDBConnector{}
}
