package kerberos

import (
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"labix.org/v2/mgo"
	"time"
)

const (
	KERBEROS_DIAL_TIMEOUT             = time.Second * 3
	KERBEROS_AUTHENTICATION_MECHANISM = "GSSAPI"
)

type KerberosDBConnector struct {
	dialInfo *mgo.DialInfo
}

// Configure the db connector. Parses the connection string and sets up
// the dial info with the default dial timeout.
func (self *KerberosDBConnector) Configure(opts *options.ToolOptions) error {

	// create the addresses to be used to connect
	connectionAddrs := util.CreateConnectionAddrs(opts.Host, opts.Port)

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Addrs:   connectionAddrs,
		Timeout: KERBEROS_DIAL_TIMEOUT,

		Username: opts.Auth.Username,
		// This should always be '$external', but legacy tools require this
		Source:    opts.Auth.Source,
		Service:   opts.Kerberos.Service,
		Mechanism: KERBEROS_AUTHENTICATION_MECHANISM,
	}

	return nil
}

// Dial the database.
func (self *KerberosDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}
