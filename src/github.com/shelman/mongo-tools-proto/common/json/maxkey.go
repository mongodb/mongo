package json

// Transition functions for recognizing MaxKey.
// Adapted from encoding/json/scanner.go.

// stateUpperMa is the state after reading `Ma`.
func stateUpperMa(s *scanner, c int) int {
	if c == 'x' {
		s.step = stateUpperMax
		return scanContinue
	}
	return s.error(c, "in literal MaxKey (expecting 'x')")
}

// stateUpperMax is the state after reading `Max`.
func stateUpperMax(s *scanner, c int) int {
	if c == 'K' {
		s.step = stateUpperMaxK
		return scanContinue
	}
	return s.error(c, "in literal MaxKey (expecting 'K')")
}

// stateUpperMaxK is the state after reading `MaxK`.
func stateUpperMaxK(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateUpperMaxKe
		return scanContinue
	}
	return s.error(c, "in literal MaxKey (expecting 'e')")
}

// stateUpperMaxKe is the state after reading `MaxKe`.
func stateUpperMaxKe(s *scanner, c int) int {
	if c == 'y' {
		s.step = stateEndValue
		return scanContinue
	}
	return s.error(c, "in literal MaxKey (expecting 'y')")
}
