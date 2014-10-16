package testutil

import (
	commonOpts "github.com/mongodb/mongo-tools/common/options"
)

func GetSSLOptions() commonOpts.SSL {
	if HasTestType(SSL_TEST_TYPE) {
		return commonOpts.SSL{
			UseSSL:        true,
			SSLCAFile:     "../common/db/openssl/testdata/ca.pem",
			SSLPEMKeyFile: "../common/db/openssl/testdata/server.pem",
		}
	}

	return commonOpts.SSL{
		UseSSL: false,
	}
}
