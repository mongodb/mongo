// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package testutil

import (
	commonOpts "github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/testtype"
	"runtime"
	"strings"
)

func GetSSLArgs() []string {
	sslOpts := GetSSLOptions()
	if sslOpts.UseSSL {
		return []string{
			"--ssl",
			"--sslCAFile", sslOpts.SSLCAFile,
			"--sslPEMKeyFile", sslOpts.SSLPEMKeyFile,
		}
	}
	return nil
}

func GetSSLOptions() commonOpts.SSL {
	// Get current filename and location
	_, filename, _, _ := runtime.Caller(0)
	// Get the path to containing folder
	foldername := filename[0:strings.LastIndex(filename, "/")]
	caFile := foldername + "/../db/testdata/ca.pem"
	serverFile := foldername + "/../db/testdata/server.pem"
	if testtype.HasTestType(testtype.SSLTestType) {
		return commonOpts.SSL{
			UseSSL:        true,
			SSLCAFile:     caFile,
			SSLPEMKeyFile: serverFile,
		}
	}

	return commonOpts.SSL{
		UseSSL: false,
	}
}
