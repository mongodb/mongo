// +build cgo
// +build -darwin

package openssl

/*
#include <openssl/ssl.h>
*/
import "C"

func FIPSModeSet(mode bool) error {
	var r C.int
	if mode {
		r = C.FIPS_mode_set(1)
	} else {
		r = C.FIPS_mode_set(0)
	}
	if r != 1 {
		return errorFromErrorQueue()
	}
	return nil
}
