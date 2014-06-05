package ssl

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
	"time"
)

var (
	DefaultSSLDialTimeout = time.Second * 3
)

// For connecting to the database over ssl
type SSLDBConnector struct {
	dialInfo *mgo.DialInfo
}

func (self *SSLDBConnector) Configure(opts *options.ToolOptions) error {

	// create the addresses to be used to connect
	connectionAddrs := util.CreateConnectionAddrs(opts.Host, opts.Port)

	// create the dialer func that will be used to connect
	dialer, err := createDialerFunc(opts)
	if err != nil {
		return err
	}

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Addrs:      connectionAddrs,
		Timeout:    DefaultSSLDialTimeout,
		DialServer: dialer,
	}

	return nil

}

func (self *SSLDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}

// To be handed to mgo.DialInfo for connecting to the server.
type dialerFunc func(addr *mgo.ServerAddr) (net.Conn, error)

// Create the dialing function that will be passed into the DialInfo for
// connecting to the server, based on the tool options specified.
func createDialerFunc(opts *options.ToolOptions) (dialerFunc, error) {

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
		/*
			if err != nil {
				util.Printlnf("Error dialing %v: %v", addr.String(), err)
			}
		*/
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
