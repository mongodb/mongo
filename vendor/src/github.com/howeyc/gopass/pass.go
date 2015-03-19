package gopass

import (
	"os"
)

// getPasswd returns the input read from terminal.
// If masked is true, typing will be matched by asterisks on the screen.
// Otherwise, typing will echo nothing.
func getPasswd(masked bool) []byte {
	var pass, bs, mask []byte
	if masked {
		bs = []byte("\b \b")
		mask = []byte("*")
	}

	for {
		if v := getch(); v == 127 || v == 8 {
			if l := len(pass); l > 0 {
				pass = pass[:l-1]
				os.Stdout.Write(bs)
			}
		} else if v == 13 || v == 10 {
			break
		} else if v != 0 {
			pass = append(pass, v)
			os.Stdout.Write(mask)
		}
	}
	println()
	return pass
}

// GetPasswd returns the password read from the terminal without echoing input.
// The returned byte array does not include end-of-line characters.
func GetPasswd() []byte {
	return getPasswd(false)
}

// GetPasswdMasked returns the password read from the terminal, echoing asterisks.
// The returned byte array does not include end-of-line characters.
func GetPasswdMasked() []byte {
	return getPasswd(true)
}
