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
