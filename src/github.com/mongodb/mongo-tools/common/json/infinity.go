package json

// Transition functions for recognizing Infinity.
// Adapted from encoding/json/scanner.go.

// stateI is the state after reading `I`.
func stateI(s *scanner, c int) int {
	if c == 'n' {
		s.step = generateState("Infinity", []byte("finity"), stateEndValue)
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'n')")
}
