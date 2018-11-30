// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

// Transition functions for recognizing single-quoted strings.
// Adapted from encoding/json/scanner.go.

// stateInSingleQuotedString is the state after reading `'`.
func stateInSingleQuotedString(s *scanner, c int) int {
	if c == '\'' {
		s.step = stateEndValue
		return scanContinue
	}
	if c == '\\' {
		s.step = stateInSingleQuotedStringEsc
		return scanContinue
	}
	if c < 0x20 {
		return s.error(c, "in string literal")
	}
	return scanContinue
}

// stateInSingleQuotedStringEsc is the state after reading `'\` during a quoted string.
func stateInSingleQuotedStringEsc(s *scanner, c int) int {
	switch c {
	case 'b', 'f', 'n', 'r', 't', '\\', '/', '\'':
		s.step = stateInSingleQuotedString
		return scanContinue
	}
	if c == 'u' {
		s.step = stateInSingleQuotedStringEscU
		return scanContinue
	}
	return s.error(c, "in string escape code")
}

// stateInSingleQuotedStringEscU is the state after reading `'\u` during a quoted string.
func stateInSingleQuotedStringEscU(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInSingleQuotedStringEscU1
		return scanContinue
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}

// stateInSingleQuotedStringEscU1 is the state after reading `'\u1` during a quoted string.
func stateInSingleQuotedStringEscU1(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInSingleQuotedStringEscU12
		return scanContinue
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}

// stateInSingleQuotedStringEscU12 is the state after reading `'\u12` during a quoted string.
func stateInSingleQuotedStringEscU12(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInSingleQuotedStringEscU123
		return scanContinue
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}

// stateInSingleQuotedStringEscU123 is the state after reading `'\u123` during a quoted string.
func stateInSingleQuotedStringEscU123(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInSingleQuotedString
		return scanContinue
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}
