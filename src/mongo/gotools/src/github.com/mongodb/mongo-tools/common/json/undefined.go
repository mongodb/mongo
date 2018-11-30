// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

// Transition functions for recognizing undefined.
// Adapted from encoding/json/scanner.go.

// stateU is the state after reading `u`.
func stateU(s *scanner, c int) int {
	if c == 'n' {
		s.step = generateState("undefined", []byte("defined"), stateEndValue)
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'n')")
}
