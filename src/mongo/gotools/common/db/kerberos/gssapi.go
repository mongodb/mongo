// Package kerberos implements connection to MongoDB using kerberos.
package kerberos

// #cgo windows CFLAGS: -Ic:/sasl/include
// #cgo windows LDFLAGS: -Lc:/sasl/lib

import (
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"time"
)

const (
	KERBEROS_AUTHENTICATION_MECHANISM = "GSSAPI"
)

type KerberosDBConnector struct {
	dialInfo *mgo.DialInfo
}

// Configure the db connector. Parses the connection string and sets up
// the dial info with the default dial timeout.
func (self *KerberosDBConnector) Configure(opts options.ToolOptions) error {

	// create the addresses to be used to connect
	connectionAddrs := util.CreateConnectionAddrs(opts.Host, opts.Port)

	timeout := time.Duration(opts.Timeout) * time.Second

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Addrs:          connectionAddrs,
		Timeout:        timeout,
		Direct:         opts.Direct,
		ReplicaSetName: opts.ReplicaSetName,

		// Kerberos principal
		Username: opts.Auth.Username,
		// Note: Password is only used on Windows. SASL doesn't allow you to specify
		// a password, so this field is ignored on Linux and OSX. Run the kinit
		// command to get a ticket first.
		Password: opts.Auth.Password,
		// This should always be '$external', but legacy tools still allow you to
		// specify a source DB
		Source:      opts.Auth.Source,
		Service:     opts.Kerberos.Service,
		ServiceHost: opts.Kerberos.ServiceHost,
		Mechanism:   KERBEROS_AUTHENTICATION_MECHANISM,
	}

	return nil
}

// Dial the database.
func (self *KerberosDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}
