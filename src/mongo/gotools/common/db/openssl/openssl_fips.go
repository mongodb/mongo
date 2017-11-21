// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// +build ssl
// +build -darwin

package openssl

import "github.com/spacemonkeygo/openssl"

func init() { sslInitializationFunctions = append(sslInitializationFunctions, SetUpFIPSMode) }

func SetUpFIPSMode(opts *ToolOptions) error {
	if err := openssl.FIPSModeSet(opts.SSLFipsMode); err != nil {
		return fmt.Errorf("couldn't set FIPS mode to %v: %v", opts.SSLFipsMode, err)
	}
	return nil
}
