package json

// Transition functions for recognizing NaN.
// Adapted from encoding/json/scanner.go.

// stateUpperNa is the state after reading `Na`.
func stateUpperNa(s *scanner, c int) int {
	if c == 'N' {
		s.step = stateEndValue
		return scanContinue
	}
	return s.error(c, "in literal NaN (expecting 'N')")
}
