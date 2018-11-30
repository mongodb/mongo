// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package common contains subpackages that are shared amongst the mongo
// tools.
package common

import (
	"strings"
)

// SplitNamespace returns the db and column from a single namespace string.
func SplitNamespace(ns string) (string, string) {
	i := strings.Index(ns, ".")
	if i != -1 {
		return ns[:i], ns[i+1:]
	}
	return "", ns
}
