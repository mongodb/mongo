package testserver

import (
	"os"
)

func (ts *TestServer) ProcessTest() *os.Process {
	if ts.server == nil {
		return nil
	}
	return ts.server.Process
}
