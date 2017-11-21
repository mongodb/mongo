// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

// Transition function for recognizing unquoted strings.
// Adapted from encoding/json/scanner.go.

func isBeginUnquotedString(c int) bool {
	return c == '$' || c == '_' || 'a' <= c && c <= 'z' || 'A' <= c && c <= 'Z'
}

func isInUnquotedString(c int) bool {
	return isBeginUnquotedString(c) || '0' <= c && c <= '9'
}

func stateInUnquotedString(s *scanner, c int) int {
	if isInUnquotedString(c) {
		return scanContinue
	}
	return stateEndValue(s, c)
}

// Decoder function that immediately returns an already unquoted string.
// Adapted from encoding/json/decode.go.
func maybeUnquoteBytes(s []byte) ([]byte, bool) {
	if len(s) == 0 {
		return nil, false
	}
	if s[0] != '"' && s[len(s)-1] != '"' && s[0] != '\'' && s[len(s)-1] != '\'' {
		return s, true
	}
	return unquoteBytes(s)
}
