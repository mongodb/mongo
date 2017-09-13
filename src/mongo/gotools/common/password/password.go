// Package password handles cleanly reading in a user's password from
// the command line. This varies heavily between operating systems.
package password

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	"os"
)

// key constants
const (
	backspaceKey      = 8
	deleteKey         = 127
	eotKey            = 3
	eofKey            = 4
	newLineKey        = 10
	carriageReturnKey = 13
)

// Prompt displays a prompt asking for the password and returns the
// password the user enters as a string.
func Prompt() string {
	var pass string
	if IsTerminal() {
		log.Logv(log.DebugLow, "standard input is a terminal; reading password from terminal")
		fmt.Fprintf(os.Stderr, "Enter password:")
		pass = GetPass()
	} else {
		log.Logv(log.Always, "reading password from standard input")
		fmt.Fprintf(os.Stderr, "Enter password:")
		pass = readPassFromStdin()
	}
	fmt.Fprintln(os.Stderr)
	return pass
}

// readPassFromStdin pipes in a password from stdin if
// we aren't using a terminal for standard input
func readPassFromStdin() string {
	pass := []byte{}
	for {
		var chBuf [1]byte
		n, err := os.Stdin.Read(chBuf[:])
		if err != nil {
			panic(err)
		}
		if n == 0 {
			break
		}
		ch := chBuf[0]
		if ch == backspaceKey || ch == deleteKey {
			if len(pass) > 0 {
				pass = pass[:len(pass)-1]
			}
		} else if ch == carriageReturnKey || ch == newLineKey || ch == eotKey || ch == eofKey {
			break
		} else if ch != 0 {
			pass = append(pass, ch)
		}
	}
	return string(pass)
}
