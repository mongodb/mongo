// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package testutil

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/options"
	"os"
	"runtime"
)

var (
	WinKerberosPwdEnv = "MONGODB_KERBEROS_PASSWORD"
)

func GetKerberosOptions() (*options.ToolOptions, error) {
	opts := &options.ToolOptions{
		Namespace: &options.Namespace{
			DB:         "kerberos",
			Collection: "test",
		},
		SSL: &options.SSL{},
		Auth: &options.Auth{
			Username:  "drivers@LDAPTEST.10GEN.CC",
			Source:    "$external",
			Mechanism: "GSSAPI",
		},
		Kerberos: &options.Kerberos{},
		Connection: &options.Connection{
			Host: "ldaptest.10gen.cc",
			Port: "27017",
		},
	}

	if runtime.GOOS == "windows" {
		opts.Auth.Password = os.Getenv(WinKerberosPwdEnv)
		if opts.Auth.Password == "" {
			return nil, fmt.Errorf("Need to set %v environment variable to run "+
				"kerberos tests on windows", WinKerberosPwdEnv)
		}
	}

	return opts, nil
}
