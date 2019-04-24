// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

// Transition functions for recognizing NaN.
// Adapted from encoding/json/scanner.go.

// stateUpperNa is the state after reading `Na`.
func stateUpperNa(s *scanner, c int) int {
	if c == 'N' {
		s.step = stateEndValue
		return scanContinue
	}
	return s.error(c, "in literal NaN (expecting 'N')")
}
