package db

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/options"
	"labix.org/v2/mgo"
	"time"
)

var (
	url           string
	masterSession *mgo.Session
	globalOptions *options.MongoToolOptions
	dialTimeout   = 3 * time.Second
)

// Configure the connection to the mongod, based on the options passed in,
// without testing the connection.
func Configure(opts *options.MongoToolOptions) error {

	// cache the options
	globalOptions = opts

	// set up the host and port
	url = opts.Host
	if opts.Port != "" {
		url += ":" + opts.Port
	}

	// TODO: validate

	return nil
}

// Confirm whether the db can be reached.
func ConfirmConnect() error {
	session, err := mgo.DialWithTimeout(url, 1*time.Second)
	if err != nil {
		return err
	}
	session.Close()
	return nil
}

func Url() string {
	return url
}

func GetSession() (*mgo.Session, error) {
	if masterSession == nil {
		if err := createMasterSession(); err != nil {
			return nil, fmt.Errorf("error connecting to db: %v", err)
		}
	}
	return masterSession.Copy(), nil
}

// Create the master session for pooling, authenticating as necessary
func createMasterSession() error {

	// make sure Configure has been called
	if globalOptions == nil {
		return fmt.Errorf("database connection has not been configured yet")
	}

	// init the master session
	var err error
	masterSession, err = mgo.DialWithTimeout(url, dialTimeout)
	if err != nil {
		return err
	}

	// authenticate, if necessary
	if globalOptions.Username != "" {

		// log in
		if err := masterSession.DB("admin").Login(globalOptions.Username,
			globalOptions.Password); err != nil {
			return fmt.Errorf("error logging in to admin database: %v", err)
		}

	}

	return nil
}
