package openssl

import (
	"errors"
	"unsafe"
)

/*
#include <stdio.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <openssl/safestack.h>

extern int _setupSystemCA(SSL_CTX* context, char * err, size_t err_len);
*/
import "C"

func (c *Ctx) SetupSystemCA() error {
	err_buf := make([]byte, 1024, 1024)
	cstr := (*C.char)(unsafe.Pointer(&err_buf[0]))
	r := C._setupSystemCA(c.ctx, cstr, 1024)
	if r == 1 {
		return nil
	}
	return errors.New(string(err_buf))
}
