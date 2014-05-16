package mgo

import (
	"time"
)

func HackSocketsPerServer(newLimit int) (restore func()) {
	oldLimit := newLimit
	restore = func() {
		socketsPerServer = oldLimit
	}
	socketsPerServer = newLimit
	return
}

func HackPingDelay(newDelay time.Duration) (restore func()) {
	oldDelay := pingDelay
	restore = func() {
		pingDelay = oldDelay
	}
	pingDelay = newDelay
	return
}

func HackSyncSocketTimeout(newTimeout time.Duration) (restore func()) {
	oldTimeout := syncSocketTimeout
	restore = func() {
		syncSocketTimeout = oldTimeout
	}
	syncSocketTimeout = newTimeout
	return
}
