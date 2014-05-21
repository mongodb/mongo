package db

import (
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/options"
	"io/ioutil"
	"labix.org/v2/mgo"
	"net"
	"time"
)

var (
	url           string
	masterSession *mgo.Session
	globalOptions *options.MongoToolOptions
	dialTimeout   = 3 * time.Second
	rootCerts     *x509.CertPool

	// the dial info to use for connecting
	dialInfo *mgo.DialInfo
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

	// create the dial info to use when connecting
	dialInfo = &mgo.DialInfo{
		Addrs:   []string{url}, // TODO: change if repl set?
		Timeout: dialTimeout,
		Source:  "admin",
	}

	if globalOptions.Username != "" {
		dialInfo.Mechanism = "MONGODB-CR"
		dialInfo.Username = globalOptions.Username
		dialInfo.Password = globalOptions.Password
	}

	// configure ssl, if necessary
	// TODO: errs, validate
	if globalOptions.SSL {
		dialInfo.Mechanism = "MONGODB-X509"
		dialInfo.DialServer = dialWithSSL

		// read in the certificate authority file and add it to the cert chain
		rootCert, err := ioutil.ReadFile(globalOptions.SSLCAFile)
		if err != nil {
			return fmt.Errorf("error reading certificate authority file: %v",
				err)
		}

		// TODO: support nil, blah-blah
		rootCerts = x509.NewCertPool()
		if !rootCerts.AppendCertsFromPEM([]byte(rootCert)) {
			return fmt.Errorf("error creating cert: %v", err)
		}

		// TODO: other files...
	}

	// TODO: validate

	return nil
}

// Custom dialer for the DialServer field of the mgo.DialInfo struct, set up
// to use ssl
func dialWithSSL(addr *mgo.ServerAddr) (net.Conn, error) {

	config := &tls.Config{}
	if globalOptions.SSLAllowInvalidCertificates {
		config.InsecureSkipVerify = true
	}
	config.RootCAs = rootCerts

	return tls.Dial("tcp", addr.String(), config)
}

// Confirm whether the db can be reached.
func ConfirmConnect() error {
	session, err := mgo.DialWithInfo(dialInfo)
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
	masterSession, err = mgo.DialWithInfo(dialInfo)
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
