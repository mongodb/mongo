package db

import (
	"gopkg.in/mgo.v2"
)

// VanillaTestWrapper allows black box test access to private fields
type VanillaTestWrapper VanillaDBConnector

func (v VanillaTestWrapper) DialInfo() *mgo.DialInfo {
	return v.dialInfo
}
