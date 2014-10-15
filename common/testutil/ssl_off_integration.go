// +build !ssltest

package testutil

import (
	commonOpts "github.com/mongodb/mongo-tools/common/options"
)

func GetSSLOptions() commonOpts.SSL {
	return commonOpts.SSL{
		UseSSL: false,
	}
}
