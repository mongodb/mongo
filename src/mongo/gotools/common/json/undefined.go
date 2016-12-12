package json

// Transition functions for recognizing undefined.
// Adapted from encoding/json/scanner.go.

// stateU is the state after reading `u`.
func stateU(s *scanner, c int) int {
	if c == 'n' {
		s.step = generateState("undefined", []byte("defined"), stateEndValue)
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'n')")
}
