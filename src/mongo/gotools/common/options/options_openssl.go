// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// +build ssl,!openssl_pre_1.0

package options

import "github.com/10gen/openssl"

func init() {
	versionInfos = append(versionInfos, versionInfo{
		key:   "OpenSSL version",
		value: openssl.Version,
	})
}
