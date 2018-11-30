// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// +build ssl

package options

func init() {
	ConnectionOptFunctions = append(ConnectionOptFunctions, registerSSLOptions)
}

func registerSSLOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("ssl options", "", self.SSL)
	if err != nil {
		return err
	}
	if self.enabledOptions.URI {
		self.URI.AddKnownURIParameters(KnownURIOptionsSSL)
	}
	BuiltWithSSL = true
	return nil
}
