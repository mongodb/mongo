package json

// Transition functions for recognizing MinKey.
// Adapted from encoding/json/scanner.go.

// stateUpperMi is the state after reading `Mi`.
func stateUpperMi(s *scanner, c int) int {
	if c == 'n' {
		s.step = generateState("MinKey", []byte("Key"), stateOptionalConstructor)
		return scanContinue
	}
	return s.error(c, "in literal MinKey (expecting 'n')")
}
