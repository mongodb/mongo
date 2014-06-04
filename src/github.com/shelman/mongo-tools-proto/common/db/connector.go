package db

import (
	"github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"labix.org/v2/mgo"
	"time"
)

// Interface type for connecting to the database.
type DBConnector interface {
	// configure, based on the opts passed in
	Configure(*options.ToolOptions) error

	// dial the database and get a fresh new session
	GetNewSession() (*mgo.Session, error)
}

// Basic connector for dialing the database, with no auth.
type VanillaDBConnector struct {
	dialInfo *mgo.DialInfo
}

func (self *VanillaDBConnector) Configure(opts *options.ToolOptions) error {

	// create the addresses to be used to connect
	connectionAddrs := util.CreateConnectionAddrs(opts.Host, opts.Port)

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Addrs:   connectionAddrs,
		Timeout: time.Second * 3,
	}

	return nil
}

func (self *VanillaDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}
