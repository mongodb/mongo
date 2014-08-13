package json

// Transition functions for recognizing undefined.
// Adapted from encoding/json/scanner.go.

// stateU is the state after reading `u`.
func stateU(s *scanner, c int) int {
	if c == 'n' {
		s.step = stateUn
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'n')")
}

// stateUn is the state after reading `un`.
func stateUn(s *scanner, c int) int {
	if c == 'd' {
		s.step = stateUnd
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'd')")
}

// stateUnd is the state after reading `und`.
func stateUnd(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateUnde
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'e')")
}

// stateUnde is the state after reading `unde`.
func stateUnde(s *scanner, c int) int {
	if c == 'f' {
		s.step = stateUndef
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'f')")
}

// stateUndef is the state after reading `undef`.
func stateUndef(s *scanner, c int) int {
	if c == 'i' {
		s.step = stateUndefi
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'i')")
}

// stateUndefi is the state after reading `undefi`.
func stateUndefi(s *scanner, c int) int {
	if c == 'n' {
		s.step = stateUndefin
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'n')")
}

// stateUndefin is the state after reading `undefin`.
func stateUndefin(s *scanner, c int) int {
	if c == 'e' {
		s.step = stateUndefine
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'e')")
}

// stateUndefine is the state after reading `undefine`.
func stateUndefine(s *scanner, c int) int {
	if c == 'd' {
		s.step = stateEndValue
		return scanContinue
	}
	return s.error(c, "in literal undefined (expecting 'd')")
}
