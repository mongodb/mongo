// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

// Transition functions for recognizing Infinity.
// Adapted from encoding/json/scanner.go.

// stateI is the state after reading `In`.
func stateIn(s *scanner, c int) int {
	if c == 'f' {
		s.step = generateState("Infinity", []byte("inity"), stateEndValue)
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'f')")
}
