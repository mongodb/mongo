package testutil

import (
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testtype"
)

func GetSSLOptions() commonOpts.SSL {
	if testtype.HasTestType(testtype.SSLTestType) {
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
