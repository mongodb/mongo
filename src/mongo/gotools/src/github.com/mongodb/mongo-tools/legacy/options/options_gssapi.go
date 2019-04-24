// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// +build sasl

package options

func init() {
	ConnectionOptFunctions = append(ConnectionOptFunctions, registerGSSAPIOptions)
}

func registerGSSAPIOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("kerberos options", "", self.Kerberos)
	if err != nil {
		return err
	}
	self.URI.AddKnownURIParameters(KnownURIOptionsKerberos)
	BuiltWithGSSAPI = true
	return nil
}
