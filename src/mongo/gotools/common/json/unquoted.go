package json

// Transition function for recognizing unquoted strings.
// Adapted from encoding/json/scanner.go.

func isBeginUnquotedString(c int) bool {
	return c == '$' || c == '_' || 'a' <= c && c <= 'z' || 'A' <= c && c <= 'Z'
}

func isInUnquotedString(c int) bool {
	return isBeginUnquotedString(c) || '0' <= c && c <= '9'
}

func stateInUnquotedString(s *scanner, c int) int {
	if isInUnquotedString(c) {
		return scanContinue
	}
	return stateEndValue(s, c)
}

// Decoder function that immediately returns an already unquoted string.
// Adapted from encoding/json/decode.go.
func maybeUnquoteBytes(s []byte) ([]byte, bool) {
	if len(s) == 0 {
		return nil, false
	}
	if s[0] != '"' && s[len(s)-1] != '"' && s[0] != '\'' && s[len(s)-1] != '\'' {
		return s, true
	}
	return unquoteBytes(s)
}
