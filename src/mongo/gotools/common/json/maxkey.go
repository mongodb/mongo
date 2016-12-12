package json

// Transition functions for recognizing MaxKey.
// Adapted from encoding/json/scanner.go.

// stateUpperMa is the state after reading `Ma`.
func stateUpperMa(s *scanner, c int) int {
	if c == 'x' {
		s.step = generateState("MaxKey", []byte("Key"), stateOptionalConstructor)
		return scanContinue
	}
	return s.error(c, "in literal MaxKey (expecting 'x')")
}
