// +build !ssltest

package mongofiles

import (
	commonOpts "github.com/mongodb/mongo-tools/common/options"
)

var (
	SSL_TEST_OPTIONS = commonOpts.SSL{
		UseSSL: false,
	}
)
