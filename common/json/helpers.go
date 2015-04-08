package json

import "fmt"

// Returns true if the byte array represents the null literal,
// and false otherwise. Assumes that `nu` is sufficient to distinguish
// between these cases.
func isNull(s []byte) bool {
	return len(s) > 1 && s[0] == 'n' && s[1] == 'u'
}

// Returns true if the byte array represents some kind of number literal,
// e.g. +123, -0.456, NaN, or Infinity, and false otherwise. Assumes that
// the first character is sufficient to distinguish between these cases
// with the exception of `N` where the second letter must be checked.
func isNumber(s []byte) bool {
	if len(s) == 0 {
		return false
	}
	if len(s) > 1 && (s[0] == 'N' && s[1] == 'a') || (s[0] == 'I' && s[1] == 'n') { // NaN
		return true
	}
	return s[0] == '+' || s[0] == '-' || s[0] == '.' || (s[0] >= '0' && s[0] <= '9')
}

// Returns true if the string represents the start of a hexadecimal
// literal, e.g. 0X123, -0x456, +0x789.
func isHexPrefix(s string) bool {
	if len(s) > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') {
		return true
	}
	return (s[0] == '+' || s[0] == '-') && isHexPrefix(s[1:])
}

// Returns the accept state (transition function) if x is empty.
// Otherwise returns a function that upon matching the first element
// of x will generate another function to match the second, etc.
// (or accept if no remaining elements).
func generateState(name string, x []byte, accept func(*scanner, int) int) func(*scanner, int) int {
	if len(x) == 0 {
		return accept
	}

	return func(s *scanner, c int) int {
		if c == int(x[0]) {
			s.step = generateState(name, x[1:], accept)
			return scanContinue
		}
		return s.error(c, fmt.Sprintf("in literal %v (expecting '%v')", name, string(x[0])))
	}
}

// stateOptionalConstructor is the state where there is the possibility of entering an empty constructor.
func stateOptionalConstructor(s *scanner, c int) int {
	if c <= ' ' && isSpace(rune(c)) {
		return scanContinue
	}
	if c == '(' {
		s.step = stateInParen
		return scanContinue
	}
	return stateEndValue(s, c)
}

// stateInParen is the state when inside a `(` waiting for a `)`
func stateInParen(s *scanner, c int) int {
	if c <= ' ' && isSpace(rune(c)) {
		return scanContinue
	}
	if c == ')' {
		s.step = stateEndValue
		return scanContinue
	}
	return s.error(c, "expecting ')' as next character")

}
