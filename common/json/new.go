package json

import (
	"fmt"
	"reflect"
)

// Transition functions for recognizing new.
// Adapted from encoding/json/scanner.go.

// stateNe is the state after reading `ne`.
func stateNe(s *scanner, c int) int {
	if c == 'w' {
		s.step = stateNew
		return scanContinue
	}
	return s.error(c, "in literal new (expecting 'w')")
}

// stateNew is the state after reading `new`.
// Ensures that there is a space after the new keyword.
func stateNew(s *scanner, c int) int {
	if c <= ' ' && isSpace(rune(c)) {
		s.step = stateBeginObjectValue
		return scanContinue
	}
	return s.error(c, "expected space after new keyword")
}

// stateBeginObjectValue is the state after reading `new`.
func stateBeginObjectValue(s *scanner, c int) int {
	if c <= ' ' && isSpace(rune(c)) {
		return scanSkipSpace
	}
	switch c {
	case 'B': // beginning of BinData
		s.step = stateB
	case 'D': // beginning of Date
		s.step = stateD
	case 'N': // beginning of NumberInt or NumberLong
		s.step = stateNumberUpperN
	case 'O': // beginning of ObjectId
		s.step = stateO
	case 'R': // beginning of RegExp
		s.step = stateR
	case 'T': // beginning of Timestamp
		s.step = stateUpperT
	default:
		return s.error(c, "looking for beginning of value")
	}

	return scanBeginLiteral
}

// stateNumberUpperN is the state after reading `N`.
func stateNumberUpperN(s *scanner, c int) int {
	if c == 'u' {
		s.step = stateUpperNu
		return scanContinue
	}
	return s.error(c, "in literal NumberInt or NumberLong (expecting 'u')")
}

// Decodes a literal stored in the underlying byte data into v.
func (d *decodeState) storeNewLiteral(v reflect.Value, fromQuoted bool) {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginLiteral {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Read constructor identifier
	start := d.off - 1
	op = d.scanWhile(scanContinue)
	if op != scanBeginCtor {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}

	// Back up so d.ctor can have the byte we just read.
	d.off--
	d.scan.undo(op)

	d.literalStore(d.data[start:d.off-1], v, fromQuoted)
}

// Returns a literal from the underlying byte data.
func (d *decodeState) getNewLiteral() interface{} {
	op := d.scanWhile(scanSkipSpace)
	if op != scanBeginLiteral {
		d.error(fmt.Errorf("expected beginning of constructor"))
	}
	return d.literalInterface()
}
