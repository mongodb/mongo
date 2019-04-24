// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

// Transition functions for recognizing MaxKey.
// Adapted from encoding/json/scanner.go.

// stateUpperMa is the state after reading `Ma`.
func stateUpperMa(s *scanner, c int) int {
	if c == 'x' {
		s.step = generateState("MaxKey", []byte("Key"), stateOptionalConstructor)
		return scanContinue
	}
	return s.error(c, "in literal MaxKey (expecting 'x')")
}
