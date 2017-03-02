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
