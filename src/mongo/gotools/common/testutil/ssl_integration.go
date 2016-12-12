package testutil

import (
	commonOpts "github.com/mongodb/mongo-tools/common/options"
)

func GetSSLOptions() commonOpts.SSL {
	if HasTestType(SSLTestType) {
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
