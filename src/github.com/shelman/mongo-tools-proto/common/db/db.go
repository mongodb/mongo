package db

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/options"
	"labix.org/v2/mgo"
	"sync"
	"time"
)

const (
	// authentication types to be passed to the driver
	AUTH_STANDARD = "MONGODB-CR"
	AUTH_SSL      = "MONGODB-X509"
)

var (
	// timeout for dialing the database server
	DialTimeout = 3 * time.Second
)

// Used to manage database sessions
type SessionProvider struct {

	// used to avoid a race condition around creating the master session
	masterSessionLock sync.Mutex

	// info for dialing mongodb
	dialInfo *mgo.DialInfo

	// the master session to use for connection pooling
	masterSession *mgo.Session
}

// Initialize a session provider to connect to the database server, based on
// the options passed in.  Returns a fully initialized provider.
func InitSessionProvider(opts *options.MongoToolOptions) (*SessionProvider,
	error) {

	// create the provider
	provider := &SessionProvider{}

	// create the addresses to be used to connect
	connectionAddrs, err := createConnectionAddrs(opts)
	if err != nil {
		return nil, fmt.Errorf("error building server connection addresses:"+
			" %v", err)
	}

	// create the necessary dial info
	provider.dialInfo = &mgo.DialInfo{
		Addrs:   connectionAddrs,
		Timeout: DialTimeout,
	}

	return provider, nil
}

// Using the options passed in, build the slice of addresses to be used to
// connect to the db server.
func createConnectionAddrs(opts *options.MongoToolOptions) ([]string, error) {
	// TODO: repl setting, more validation

	url := opts.Host
	if opts.Port != "" {
		url += ":" + opts.Port
	}

	return []string{url}, nil
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
			self.masterSession, err = mgo.DialWithInfo(self.dialInfo)
			if err != nil {
				return nil, fmt.Errorf("error connecting to db server: %v", err)
			}
		}
	}

	// copy and return the master session
	return self.masterSession.Copy(), nil
}
