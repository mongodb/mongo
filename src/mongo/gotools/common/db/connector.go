package db

import (
	"time"

	"github.com/mongodb/mongo-tools/common/db/kerberos"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
)

// Interface type for connecting to the database.
type DBConnector interface {
	// configure, based on the options passed in
	Configure(options.ToolOptions) error

	// dial the database and get a fresh new session
	GetNewSession() (*mgo.Session, error)
}

// Basic connector for dialing the database, with no authentication.
type VanillaDBConnector struct {
	dialInfo *mgo.DialInfo
}

// Configure sets up the db connector using the options in opts. It parses the
// connection string and then sets up the dial information using the default
// dial timeout.
func (self *VanillaDBConnector) Configure(opts options.ToolOptions) error {
	// create the addresses to be used to connect
	connectionAddrs := util.CreateConnectionAddrs(opts.Host, opts.Port)

	timeout := time.Duration(opts.Timeout) * time.Second

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Addrs:          connectionAddrs,
		Timeout:        timeout,
		Direct:         opts.Direct,
		ReplicaSetName: opts.ReplicaSetName,
		Username:       opts.Auth.Username,
		Password:       opts.Auth.Password,
		Source:         opts.GetAuthenticationDatabase(),
		Mechanism:      opts.Auth.Mechanism,
	}
	kerberos.AddKerberosOpts(opts, self.dialInfo)
	return nil
}

// GetNewSession connects to the server and returns the established session and any
// error encountered.
func (self *VanillaDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}
