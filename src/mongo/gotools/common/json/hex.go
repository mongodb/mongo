// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

// Transition function for recognizing hexadecimal numbers.
// Adapted from encoding/json/scanner.go.

// stateHex is the state after reading `0x` or `0X`.
func stateHex(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateHex
		return scanContinue
	}
	return stateEndValue(s, c)
}
