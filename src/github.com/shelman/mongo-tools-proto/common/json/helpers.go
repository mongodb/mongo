package json

// Returns true if the byte array represents the null literal,
// and false otherwise. Assumes that `nu` is sufficient to distinguish
// between these cases.
func isNull(s []byte) bool {
	return len(s) > 1 && s[0] == 'n' && s[1] == 'u'
}

// Returns true if the byte array represents some kind of number literal,
// e.g. +123, -0.456, NaN, or Infinity, and false otherwise. Assumes that
// the first character is sufficient to distinguish between these cases
// with the exception of `N` where the second letter must be checked.
func isNumber(s []byte) bool {
	if len(s) == 0 {
		return false
	}
	if len(s) > 1 && s[0] == 'N' && s[1] == 'a' { // NaN
		return true
	}
	return s[0] == '+' || s[0] == '-' || s[0] == '.' || s[0] == 'I' || (s[0] >= '0' && s[0] <= '9')
}

// Returns true if the string represents the start of a hexadecimal
// literal, e.g. 0X123, -0x456, +0x789.
func isHexPrefix(s string) bool {
	if len(s) > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') {
		return true
	}
	return (s[0] == '+' || s[0] == '-') && isHexPrefix(s[1:])
}
