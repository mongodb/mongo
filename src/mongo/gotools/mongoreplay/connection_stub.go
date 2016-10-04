package mongoreplay

import (
	"bytes"
	"net"
	"sync"
	"time"

	mgo "github.com/10gen/llmgo"
)

// SessionStub mocks an MongoSession by implementing the AcquireSocketPrivate
// method.  It allows for tests to pass around a struct with stubbed fields that
// can then be read later for testing.
type SessionStub struct {
	startup sync.Once
	mgo.MongoSession
	connection ConnStub
	socket     *mgo.MongoSocket
}

// ConnStub mocks the connection used by an mgo session. It implements the
// net.Conn interface so that it may be used as a connection for testing in
// llmgo It contains a write buffer and a read buffer. It writes into the write
// buffer, and reads from the read buffer so that its ends may be given in
// reverse to another function. (i.e., another function can write to its read
// buffer and it will receive this as incoming data)
type ConnStub struct {
	closed      bool
	readBuffer  *bytes.Buffer
	writeBuffer *bytes.Buffer
}

func (conn *ConnStub) Read(b []byte) (n int, err error) {
	return conn.readBuffer.Read(b)
}

func (conn *ConnStub) Write(b []byte) (n int, err error) {
	return conn.writeBuffer.Write(b)
}

// Close doesn't actually do anything, and is here to implement net.Conn.
func (conn *ConnStub) Close() error {
	return nil
}

// LocalAddr doesn't actually do anything, and is here to implement net.Conn.
func (conn *ConnStub) LocalAddr() net.Addr {
	return nil
}

// RemoteAddr doesn't actually do anything, and is here to implement net.Conn.
func (conn *ConnStub) RemoteAddr() net.Addr {
	return nil
}

// SetDeadline doesn't actually do anything, and is here to implement net.Conn.
func (conn *ConnStub) SetDeadline(t time.Time) error {
	return nil
}

// SetReadDeadline doesn't actually do anything, and is here to implement net.Conn.
func (conn *ConnStub) SetReadDeadline(t time.Time) error {
	return nil
}

// SetWriteDeadline doesn't actually do anything, and is here to implement net.Conn.
func (conn *ConnStub) SetWriteDeadline(t time.Time) error {
	return nil
}

// newTwoSidedConn makes two ConnStub's which use the same buffers but in
// opposite roles.  The read end of one buffer is handed to the other connection
// as the write end and vice versa
func newTwoSidedConn() (conn1 ConnStub, conn2 ConnStub) {
	buffer1 := &bytes.Buffer{}
	buffer2 := &bytes.Buffer{}
	conn1 = ConnStub{false, buffer1, buffer2}
	conn2 = ConnStub{false, buffer2, buffer1}
	return conn1, conn2
}

// AcquireSocketPrivate is an implementation of MongoSession's function that
// allows for the a stubbed connection to the passed to the other operations of
// llmgo for testing
func (session *SessionStub) AcquireSocketPrivate(slaveOk bool) (*mgo.MongoSocket, error) {
	session.startup.Do(func() {
		session.socket = mgo.NewDumbSocket(&session.connection)
	})
	return session.socket, nil
}
