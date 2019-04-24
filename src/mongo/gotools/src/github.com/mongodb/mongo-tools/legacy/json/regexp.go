// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package json

import (
	"fmt"
	"reflect"
	"unicode"
	"unicode/utf16"
	"unicode/utf8"
)

// Transition functions for recognizing RegExp.
// Adapted from encoding/json/scanner.go.

// stateR is the state after reading `R`.
func stateR(s *scanner, c int) int {
	if c == 'e' {
		s.step = generateState("RegExp", []byte("gExp"), stateConstructor)
		return scanContinue
	}
	return s.error(c, "in literal RegExp (expecting 'e')")
}

// stateInRegexpPattern is the state after reading `/`.
func stateInRegexpPattern(s *scanner, c int) int {
	if c == '/' {
		s.step = stateInRegexpOptions
		return scanRegexpOptions
	}
	if c == '\\' {
		s.step = stateInRegexpPatternEsc
		return scanRegexpPattern
	}
	if c < 0x20 {
		return s.error(c, "in regular expression literal")
	}
	return scanRegexpPattern
}

// stateInRegexpPatternEsc is the state after reading `'\` during a regex pattern.
func stateInRegexpPatternEsc(s *scanner, c int) int {
	switch c {
	case 'b', 'f', 'n', 'r', 't', '\\', '/', '\'':
		s.step = stateInRegexpPattern
		return scanRegexpPattern
	}
	if c == 'u' {
		s.step = stateInRegexpPatternEscU
		return scanRegexpPattern
	}
	return s.error(c, "in string escape code")
}

// stateInRegexpPatternEscU is the state after reading `'\u` during a regex pattern.
func stateInRegexpPatternEscU(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInRegexpPatternEscU1
		return scanRegexpPattern
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}

// stateInRegexpPatternEscU1 is the state after reading `'\u1` during a regex pattern.
func stateInRegexpPatternEscU1(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInRegexpPatternEscU12
		return scanRegexpPattern
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}

// stateInRegexpPatternEscU12 is the state after reading `'\u12` during a regex pattern.
func stateInRegexpPatternEscU12(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInRegexpPatternEscU123
		return scanRegexpPattern
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}

// stateInRegexpPatternEscU123 is the state after reading `'\u123` during a regex pattern.
func stateInRegexpPatternEscU123(s *scanner, c int) int {
	if '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F' {
		s.step = stateInRegexpPattern
		return scanRegexpPattern
	}
	// numbers
	return s.error(c, "in \\u hexadecimal character escape")
}

// stateInRegexpOptions is the state after reading `/foo/`.
func stateInRegexpOptions(s *scanner, c int) int {
	switch c {
	case 'g', 'i', 'm', 's':
		return scanRegexpOptions
	}
	return stateEndValue(s, c)
}

// Decodes a RegExp literal stored in the underlying byte data into v.
func (d *decodeState) storeRegexp(v reflect.Value) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args, err := d.ctor("RegExp", []reflect.Type{stringType, stringType})
	if err != nil {
		d.error(err)
	}
	switch kind := v.Kind(); kind {
	case reflect.Interface:
		arg0 := args[0].String()
		arg1 := args[1].String()
		v.Set(reflect.ValueOf(RegExp{arg0, arg1}))
	default:
		d.error(fmt.Errorf("cannot store %v value into %v type", regexpType, kind))
	}
}

// Returns a RegExp literal from the underlying byte data.
func (d *decodeState) getRegexp() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	args := d.ctorInterface()
	if err := ctorNumArgsMismatch("RegExp", 2, len(args)); err != nil {
		d.error(err)
	}
	arg0, ok := args[0].(string)
	if !ok {
		d.error(fmt.Errorf("expected string for first argument of RegExp constructor"))
	}
	arg1, ok := args[1].(string)
	if !ok {
		d.error(fmt.Errorf("expected string for second argument of RegExp constructor"))
	}
	return RegExp{arg0, arg1}
}

// Decoder function that breaks a regular expression literal into its pattern and options.
// Adapted from encoding/json/decode.go.

// regexp consumes a regular expression from d.data[d.off-1:].
// the two bytes of the regexp ("/a") have been read already.
func (d *decodeState) regexp() (string, string, error) {
	start := d.off - 1

	// Look ahead for /.
	op := d.scanWhile(scanRegexpPattern)
	if op != scanRegexpOptions {
		return "", "", fmt.Errorf("expected beginning of regular expression options")
	}
	pattern := d.data[start : d.off-1]

	start = d.off
	op = d.scanWhile(scanRegexpOptions)

	// Back up so caller can have the byte we just read.
	d.off--
	d.scan.undo(op)

	options := d.data[start:d.off]

	// Check for unusual characters. If there are none,
	// then no copying is needed, so return string of the
	// original bytes.
	r := 0
	for r < len(pattern) {
		c := pattern[r]
		if c == '\\' || c == '/' || c < ' ' {
			break
		}
		if c < utf8.RuneSelf {
			r++
			continue
		}
		rr, size := utf8.DecodeRune(pattern[r:])
		if rr == utf8.RuneError && size == 1 {
			break
		}
		r += size
	}
	if r == len(pattern) {
		return string(pattern), string(options), nil
	}

	b := make([]byte, len(pattern)+2*utf8.UTFMax)
	w := copy(b, pattern[0:r])
	for r < len(pattern) {
		// Out of room?  Can only happen if pattern is full of
		// malformed UTF-8 and we're replacing each
		// byte with RuneError.
		if w >= len(b)-2*utf8.UTFMax {
			nb := make([]byte, (len(b)+utf8.UTFMax)*2)
			copy(nb, b[0:w])
			b = nb
		}
		switch c := pattern[r]; {
		case c == '\\':
			r++
			if r >= len(pattern) {
				return "", "", errPhase
			}
			switch pattern[r] {
			default:
				return "", "", fmt.Errorf("invalid escape character")
			case '"', '\\', '/', '\'':
				b[w] = pattern[r]
				r++
				w++
			case 'b':
				b[w] = '\b'
				r++
				w++
			case 'f':
				b[w] = '\f'
				r++
				w++
			case 'n':
				b[w] = '\n'
				r++
				w++
			case 'r':
				b[w] = '\r'
				r++
				w++
			case 't':
				b[w] = '\t'
				r++
				w++
			case 'u':
				r--
				rr := getu4(pattern[r:])
				if rr < 0 {
					return "", "", fmt.Errorf("non-hexadecimal character found")
				}
				r += 6
				if utf16.IsSurrogate(rr) {
					rr1 := getu4(pattern[r:])
					if dec := utf16.DecodeRune(rr, rr1); dec != unicode.ReplacementChar {
						// A valid pair; consume.
						r += 6
						w += utf8.EncodeRune(b[w:], dec)
						break
					}
					// Invalid surrogate; fall back to replacement rune.
					rr = unicode.ReplacementChar
				}
				w += utf8.EncodeRune(b[w:], rr)
			}

		// Forward slash, control characters are invalid.
		case c == '/', c < ' ':
			d.error(fmt.Errorf("regular expression pattern cannot contain unescaped '/'"))

		// ASCII
		case c < utf8.RuneSelf:
			b[w] = c
			r++
			w++

		// Coerce to well-formed UTF-8.
		default:
			rr, size := utf8.DecodeRune(pattern[r:])
			r += size
			w += utf8.EncodeRune(b[w:], rr)
		}
	}
	return string(b[0:w]), string(options), nil
}
