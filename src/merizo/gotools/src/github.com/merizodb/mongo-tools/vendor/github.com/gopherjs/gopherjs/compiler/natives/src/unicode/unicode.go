// +build js

package unicode

func to(_case int, r rune, caseRange []CaseRange) (mappedRune rune, foundMapping bool) {
	if _case < 0 || MaxCase <= _case {
		return ReplacementChar, false
	}
	lo := 0
	hi := len(caseRange)
	for lo < hi {
		m := lo + (hi-lo)/2
		cr := &caseRange[m] // performance critical for GopherJS: get address here instead of copying the CaseRange
		if rune(cr.Lo) <= r && r <= rune(cr.Hi) {
			delta := rune(cr.Delta[_case])
			if delta > MaxRune {
				return rune(cr.Lo) + ((r-rune(cr.Lo))&^1 | rune(_case&1)), true
			}
			return r + delta, true
		}
		if r < rune(cr.Lo) {
			hi = m
		} else {
			lo = m + 1
		}
	}
	return r, false
}
