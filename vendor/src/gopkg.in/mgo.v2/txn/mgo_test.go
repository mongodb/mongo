package txn_test

import (
	"bytes"
	"gopkg.in/mgo.v2"
	. "gopkg.in/check.v1"
	"os/exec"
	"time"
)

// ----------------------------------------------------------------------------
// The mgo test suite

type MgoSuite struct {
	output  bytes.Buffer
	server  *exec.Cmd
	session *mgo.Session
}

var mgoaddr = "127.0.0.1:50017"

func (s *MgoSuite) SetUpSuite(c *C) {
	//mgo.SetDebug(true)
	mgo.SetStats(true)
	dbdir := c.MkDir()
	args := []string{
		"--dbpath", dbdir,
		"--bind_ip", "127.0.0.1",
		"--port", "50017",
		"--nssize", "1",
		"--noprealloc",
		"--smallfiles",
		"--nojournal",
		"-vvvvv",
	}
	s.server = exec.Command("mongod", args...)
	s.server.Stdout = &s.output
	s.server.Stderr = &s.output
	err := s.server.Start()
	if err != nil {
		panic(err)
	}
}

func (s *MgoSuite) TearDownSuite(c *C) {
	s.server.Process.Kill()
	s.server.Process.Wait()
}

func (s *MgoSuite) SetUpTest(c *C) {
	err := DropAll(mgoaddr)
	if err != nil {
		panic(err)
	}
	mgo.SetLogger(c)
	mgo.ResetStats()

	s.session, err = mgo.Dial(mgoaddr)
	c.Assert(err, IsNil)
}

func (s *MgoSuite) TearDownTest(c *C) {
	if s.session != nil {
		s.session.Close()
	}
	for i := 0; ; i++ {
		stats := mgo.GetStats()
		if stats.SocketsInUse == 0 && stats.SocketsAlive == 0 {
			break
		}
		if i == 20 {
			c.Fatal("Test left sockets in a dirty state")
		}
		c.Logf("Waiting for sockets to die: %d in use, %d alive", stats.SocketsInUse, stats.SocketsAlive)
		time.Sleep(500 * time.Millisecond)
	}
}

func DropAll(mongourl string) (err error) {
	session, err := mgo.Dial(mongourl)
	if err != nil {
		return err
	}
	defer session.Close()

	names, err := session.DatabaseNames()
	if err != nil {
		return err
	}
	for _, name := range names {
		switch name {
		case "admin", "local", "config":
		default:
			err = session.DB(name).DropDatabase()
			if err != nil {
				return err
			}
		}
	}
	return nil
}
