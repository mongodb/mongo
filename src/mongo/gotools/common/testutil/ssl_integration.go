// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

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
