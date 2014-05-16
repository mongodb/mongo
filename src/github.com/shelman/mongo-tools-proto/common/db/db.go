package db

import (
	"fmt"
	"labix.org/v2/mgo"
	"time"
)

var (
	mongoHost     string
	mongoPort     string
	url           string
	masterSession *mgo.Session
)

func SetHostAndPort(host string, port string) {
	mongoHost = host
	mongoPort = port
	url = mongoHost
	if mongoPort != "" {
		url += ":" + mongoPort
	}
}

func Url() string {
	return url
}

func GetSession() (*mgo.Session, error) {
	if masterSession == nil {
		var err error
		masterSession, err = mgo.DialWithTimeout(url, 5*time.Second)
		if err != nil {
			return nil, fmt.Errorf("error connecting to db: %v", err)
		}
	}
	return masterSession.Copy(), nil
}
