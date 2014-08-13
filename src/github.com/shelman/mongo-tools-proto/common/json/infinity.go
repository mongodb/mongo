package json

// Transition functions for recognizing Infinity.
// Adapted from encoding/json/scanner.go.

// stateI is the state after reading `I`.
func stateI(s *scanner, c int) int {
	if c == 'n' {
		s.step = stateIn
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'n')")
}

// stateIn is the state after reading `In`.
func stateIn(s *scanner, c int) int {
	if c == 'f' {
		s.step = stateInf
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'f')")
}

// stateInf is the state after reading `Inf`.
func stateInf(s *scanner, c int) int {
	if c == 'i' {
		s.step = stateInfi
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'i')")
}

// stateInfi is the state after reading `Infi`.
func stateInfi(s *scanner, c int) int {
	if c == 'n' {
		s.step = stateInfin
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'n')")
}

// stateInfin is the state after reading `Infin`.
func stateInfin(s *scanner, c int) int {
	if c == 'i' {
		s.step = stateInfini
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'i')")
}

// stateInfini is the state after reading `Infini`.
func stateInfini(s *scanner, c int) int {
	if c == 't' {
		s.step = stateInfinit
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 't')")
}

// stateInfinit is the state after reading `Infinit`.
func stateInfinit(s *scanner, c int) int {
	if c == 'y' {
		s.step = stateEndValue
		return scanContinue
	}
	return s.error(c, "in literal Infinity (expecting 'y')")
}
