package json

// Transition functions for recognizing MinKey.
// Adapted from encoding/json/scanner.go.

// stateUpperMi is the state after reading `Mi`.
func stateUpperMi(s *scanner, c int) int {
	if c == 'n' {
		s.step = stateUpperMin
		return scanContinue
	}
	return s.error(c, "in literal MinKey (expecting 'n')")
}

// stateUpperMin is the state after reading `Min`.
func stateUpperMin(s *scanner, c int) int {
	if c == 'K' {
		s.step = stateUpperMinK
		return scanContinue
	}
	return s.error(c, "in literal MinKey (expecting 'K')")
}

// stateUpperMinK is the state after reading `MinK`.
func stateUpperMinK(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateUpperMinKe
		return scanContinue
	}
	return s.error(c, "in literal MinKey (expecting 'e')")
}

// stateUpperMinKe is the state after reading `MinKe`.
func stateUpperMinKe(s *scanner, c int) int {
	if c == 'y' {
		s.step = stateEndValue
		return scanContinue
	}
	return s.error(c, "in literal MinKey (expecting 'y')")
}
