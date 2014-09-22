package db

import (
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"time"
)

// Interface type for connecting to the database.
type DBConnector interface {
	// configure, based on the options passed in
	Configure(options.ToolOptions) error

	// dial the database and get a fresh new session
	GetNewSession() (*mgo.Session, error)
}

var (
	DefaultDialTimeout = time.Second * 3
)

// Basic connector for dialing the database, with no authentication.
type VanillaDBConnector struct {
	dialInfo *mgo.DialInfo
}

// Configure the db connector. Parses the connection string and sets up
// the dial info with the default dial timeout.
func (self *VanillaDBConnector) Configure(opts options.ToolOptions) error {

	// create the addresses to be used to connect
	connectionAddrs := util.CreateConnectionAddrs(opts.Host, opts.Port)

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Addrs:     connectionAddrs,
		Timeout:   DefaultDialTimeout,
		Direct:    opts.Direct,
		Username:  opts.Auth.Username,
		Password:  opts.Auth.Password,
		Source:    opts.Auth.Source,
		Mechanism: opts.Auth.Mechanism,
	}

	return nil
}

// Dial the database.
func (self *VanillaDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}
