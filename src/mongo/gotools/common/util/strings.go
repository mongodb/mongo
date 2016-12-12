package util

// Pluralize takes an amount and two strings denoting the singular
// and plural noun the amount represents. If the amount is singular,
// the singular form is returned; otherwise plural is returned. E.g.
//  Pluralize(X, "mouse", "mice") -> 0 mice, 1 mouse, 2 mice, ...
func Pluralize(amount int, singular, plural string) string {
	if amount == 1 {
		return singular
	}
	return plural
}
