package db

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"io/ioutil"
	"labix.org/v2/mgo"
	"net"
	"strings"
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
	DialTimeout = 5 * time.Second
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

// Initialize a session provider to connect to the database server, based on
// the options passed in.  Returns a fully initialized provider.
func InitSessionProvider(opts *options.ToolOptions) (*SessionProvider,
	error) {

	// create the provider
	provider := &SessionProvider{}

	// create the addresses to be used to connect
	connectionAddrs := createConnectionAddrs(opts.Host, opts.Port)

	// create the function used to dial the server
	dialer, err := createDialerFunc(opts)
	if err != nil {
		return nil, fmt.Errorf("error creating dialing function for server:"+
			" %v", err)
	}

	// create the necessary dial info
	provider.dialInfo = &mgo.DialInfo{
		Addrs:      connectionAddrs,
		Timeout:    DialTimeout,
		DialServer: dialer,
	}

	return provider, nil
}

// Using the options passed in, build the slice of addresses to be used to
// connect to the db server.
func createConnectionAddrs(host, port string) []string {

	// parse the host string into the individual hosts
	addrs := parseHost(host)

	// if a port is specified, append it to all the hosts
	if port != "" {
		for idx, addr := range addrs {
			addrs[idx] = fmt.Sprintf("%v:%v", addr, port)
		}
	}

	return addrs
}

// Helper function for parsing the host string into addresses.  Returns a slice
// of the individual addresses to use to connect, as well as an error if the url
// is malformed.
func parseHost(host string) []string {

	// strip off the replica set name from the beginning
	slashIndex := strings.Index(host, "/")
	if slashIndex != -1 {
		if slashIndex == len(host)-1 {
			return []string{""}
		}
		host = host[slashIndex+1:]
	}

	// split into the individual hosts
	return strings.Split(host, ",")
}

// To be handed to mgo.DialInfo for connecting to the server.
type dialerFunc func(addr *mgo.ServerAddr) (net.Conn, error)

// Create the dialing function that will be passed into the DialInfo for
// connecting to the server, based on the tool options specified.
func createDialerFunc(opts *options.ToolOptions) (dialerFunc, error) {

	// if ssl is not being used, we can return nil and mgo will just use a
	// standard dialer
	if !opts.UseSSL {
		return nil, nil
	}

	// the tls config
	config := &tls.Config{}

	// allow invalid certs
	if opts.SSLAllowInvalid {
		config.InsecureSkipVerify = true
	}

	// add the ca file, if it is specified
	if opts.SSLCAFile != "" {
		pool := x509.NewCertPool()

		// read in the specified ca file
		fileBytes, err := ioutil.ReadFile(opts.SSLCAFile)
		if err != nil {
			return nil, fmt.Errorf("error reading in CA file %v: %v",
				opts.SSLCAFile, err)
		}

		// check if the file is a pem file
		if strings.HasSuffix(opts.SSLCAFile, ".pem") {
			if !pool.AppendCertsFromPEM(fileBytes) {
				return nil, fmt.Errorf("error adding pem-encoded ca certs"+
					" from %v: %v", opts.SSLCAFile, err)
			}
		}

		// TODO: support non-pem encoded ca files?

		// set up the root cas
		config.RootCAs = pool

	}

	// add the PEM key file with the cert and private key, if specified
	if opts.SSLPEMKeyFile != "" {

		// read in the file
		fileBytes, err := ioutil.ReadFile(opts.SSLPEMKeyFile)
		if err != nil {
			return nil, fmt.Errorf("error reading in key file %v: %v",
				opts.SSLPEMKeyFile, err)
		}

		// parse out the cert and private key
		certPEMBlock, keyPEMBlock, err := sslKeyPairFromBytes(fileBytes)
		if err != nil {
			return nil, fmt.Errorf("ssl cert / private key file %v is"+
				" malformed: %v", opts.SSLPEMKeyFile, err)
		}

		// create a new certificate
		clientCert, err := tls.X509KeyPair(certPEMBlock, keyPEMBlock)
		if err != nil {
			return nil, fmt.Errorf("error creating client certificate: %v", err)
		}

		// add the certificate in
		config.Certificates = append(config.Certificates, clientCert)

	}

	// return the dialing func
	return func(addr *mgo.ServerAddr) (net.Conn, error) {
		conn, err := tls.Dial("tcp", addr.String(), config)
		if err != nil {
			util.Printlnf("Error dialing %v: %v", addr.String(), err)
		}
		return conn, err
	}, nil

}

// Helper to parse out a pem-encoded certificate and private key from a chunk
// of bytes.  Returns the pem-encoded cert, the pem-encoded private key, and
// an error, if any occurs.
func sslKeyPairFromBytes(data []byte) ([]byte, []byte, error) {

	// to be returned
	var certPEMBlock []byte
	var keyPEMBlock []byte

	// loop, decoding
	for {
		// grab the next pem-encoded block from the data
		block, extra := pem.Decode(data)

		// if there are no more blocks, make sure we've got the two items we
		// need, and return appropriately
		if block == nil {
			if certPEMBlock != nil && keyPEMBlock != nil {
				return certPEMBlock, keyPEMBlock, nil
			} else {
				return nil, nil, fmt.Errorf("the file must contain both a" +
					" certificate and a private key")
			}
		}

		switch block.Type {
		case "CERTIFICATE":
			if certPEMBlock != nil {
				return nil, nil, fmt.Errorf("cannot specify multiple" +
					" certificates in the pem key file")
			}
			certPEMBlock = pem.EncodeToMemory(block)
		case "PRIVATE KEY":
			if keyPEMBlock != nil {
				return nil, nil, fmt.Errorf("cannot specify multiple private" +
					" keys in the pem key file")
			}
			keyPEMBlock = pem.EncodeToMemory(block)
		}

		// chop off the decoded block
		data = extra
	}

}
