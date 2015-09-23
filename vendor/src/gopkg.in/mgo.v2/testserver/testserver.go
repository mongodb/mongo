// WARNING: This package was replaced by mgo.v2/dbtest.
package testserver

import (
	"bytes"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strconv"
	"time"

	"gopkg.in/mgo.v2"
	"gopkg.in/tomb.v2"
)

// WARNING: This package was replaced by mgo.v2/dbtest.
type TestServer struct {
	session *mgo.Session
	output  bytes.Buffer
	server  *exec.Cmd
	dbpath  string
	host    string
	tomb    tomb.Tomb
}

// WARNING: This package was replaced by mgo.v2/dbtest.
func (ts *TestServer) SetPath(dbpath string) {
	ts.dbpath = dbpath
}

func (ts *TestServer) start() {
	if ts.server != nil {
		panic("TestServer already started")
	}
	if ts.dbpath == "" {
		panic("TestServer.SetPath must be called before using the server")
	}
	mgo.SetStats(true)
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic("unable to listen on a local address: " + err.Error())
	}
	addr := l.Addr().(*net.TCPAddr)
	l.Close()
	ts.host = addr.String()

	args := []string{
		"--dbpath", ts.dbpath,
		"--bind_ip", "127.0.0.1",
		"--port", strconv.Itoa(addr.Port),
		"--nssize", "1",
		"--noprealloc",
		"--smallfiles",
		"--nojournal",
	}
	ts.tomb = tomb.Tomb{}
	ts.server = exec.Command("mongod", args...)
	ts.server.Stdout = &ts.output
	ts.server.Stderr = &ts.output
	err = ts.server.Start()
	if err != nil {
		panic(err)
	}
	ts.tomb.Go(ts.monitor)
	ts.Wipe()
}

func (ts *TestServer) monitor() error {
	ts.server.Process.Wait()
	if ts.tomb.Alive() {
		// Present some debugging information.
		fmt.Fprintf(os.Stderr, "---- mongod process died unexpectedly:\n")
		fmt.Fprintf(os.Stderr, "%s", ts.output.Bytes())
		fmt.Fprintf(os.Stderr, "---- mongod processes running right now:\n")
		cmd := exec.Command("/bin/sh", "-c", "ps auxw | grep mongod")
		cmd.Stdout = os.Stderr
		cmd.Stderr = os.Stderr
		cmd.Run()
		fmt.Fprintf(os.Stderr, "----------------------------------------\n")

		panic("mongod process died unexpectedly")
	}
	return nil
}

// WARNING: This package was replaced by mgo.v2/dbtest.
func (ts *TestServer) Stop() {
	if ts.session != nil {
		ts.checkSessions()
		if ts.session != nil {
			ts.session.Close()
			ts.session = nil
		}
	}
	if ts.server != nil {
		ts.tomb.Kill(nil)
		ts.server.Process.Kill()
		select {
		case <-ts.tomb.Dead():
		case <-time.After(5 * time.Second):
			panic("timeout waiting for mongod process to die")
		}
		ts.server = nil
	}
}

// WARNING: This package was replaced by mgo.v2/dbtest.
func (ts *TestServer) Session() *mgo.Session {
	if ts.server == nil {
		ts.start()
	}
	if ts.session == nil {
		mgo.ResetStats()
		var err error
		ts.session, err = mgo.Dial(ts.host + "/test")
		if err != nil {
			panic(err)
		}
	}
	return ts.session.Copy()
}

// WARNING: This package was replaced by mgo.v2/dbtest.
func (ts *TestServer) checkSessions() {
	if check := os.Getenv("CHECK_SESSIONS"); check == "0" || ts.server == nil || ts.session == nil {
		return
	}
	ts.session.Close()
	ts.session = nil
	for i := 0; i < 100; i++ {
		stats := mgo.GetStats()
		if stats.SocketsInUse == 0 && stats.SocketsAlive == 0 {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	panic("There are mgo sessions still alive.")
}

// WARNING: This package was replaced by mgo.v2/dbtest.
func (ts *TestServer) Wipe() {
	if ts.server == nil || ts.session == nil {
		return
	}
	ts.checkSessions()
	sessionUnset := ts.session == nil
	session := ts.Session()
	defer session.Close()
	if sessionUnset {
		ts.session.Close()
		ts.session = nil
	}
	names, err := session.DatabaseNames()
	if err != nil {
		panic(err)
	}
	for _, name := range names {
		switch name {
		case "admin", "local", "config":
		default:
			err = session.DB(name).DropDatabase()
			if err != nil {
				panic(err)
			}
		}
	}
}
