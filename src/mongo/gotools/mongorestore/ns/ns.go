package ns

import (
	"fmt"
	"regexp"
	"strings"
)

// Renamer maps namespaces given user-defined patterns
type Renamer struct {
	// List of regexps to match namespaces against
	matchers []*regexp.Regexp
	// List of regexp-syle replacement strings to use with the matcher
	replacers []string
}

// Matcher identifies namespaces given user-defined patterns
type Matcher struct {
	// List of regexps to check namespaces against
	matchers []*regexp.Regexp
}

var (
	unescapeReplacements = []string{
		`\\`, `\`,
		`\*`, "*",
		`\`, "",
	}
	unescapeReplacer = strings.NewReplacer(unescapeReplacements...)
)

// Escape escapes instances of '\' and '*' with a backslash
func Escape(in string) string {
	in = strings.Replace(in, `\`, `\\`, -1)
	in = strings.Replace(in, "*", `\*`, -1)
	return in
}

// Unescape removes the escaping backslash where applicable
func Unescape(in string) string {
	return unescapeReplacer.Replace(in)
}

var (
	// Finds non-escaped asterisks
	wildCardRE = regexp.MustCompile(`^(|.*[^\\])\*(.*)$`)
	// Finds $variables$ at the beginning of a string
	variableRE = regexp.MustCompile(`^\$([^\$]*)\$(.*)$`)
	// List of control characters that a regexp can use
	escapedChars = `*[](){}\?$^+!.|`
)

// peelLeadingVariable returns the first variable in the given string and
// the remaining string if there is such a variable at the beginning
func peelLeadingVariable(in string) (name, rest string, ok bool) {
	var match = variableRE.FindStringSubmatch(in)
	if len(match) != 3 {
		return
	}
	return match[1], match[2], true
}

// replaceWildCards replaces non-escaped asterisks with named variables
// i.e. 'pre*.*' becomes 'pre$1$.$2$'
func replaceWildCards(in string) string {
	count := 1
	match := wildCardRE.FindStringSubmatch(in)
	for len(match) > 2 {
		in = fmt.Sprintf("%s$%d$%s", match[1], count, match[2])
		match = wildCardRE.FindStringSubmatch(in)
		count++
	}
	return Unescape(in)
}

// countAsterisks returns the number of non-escaped asterisks
func countAsterisks(in string) int {
	return strings.Count(in, "*") - strings.Count(in, `\*`)
}

// countDollarSigns returns the number of dollar signs
func countDollarSigns(in string) int {
	return strings.Count(in, "$")
}

// validateReplacement performs preliminary checks on the from and to strings,
// returning an error if it finds a syntactic issue
func validateReplacement(from, to string) error {
	if strings.Contains(from, "$") {
		if countDollarSigns(from)%2 != 0 {
			return fmt.Errorf("Odd number of dollar signs in from: '%s'", from)
		}
		if countDollarSigns(to)%2 != 0 {
			return fmt.Errorf("Odd number of dollar signs in to: '%s'", to)
		}
	} else {
		if countAsterisks(from) != countAsterisks(to) {
			return fmt.Errorf("Different number of asterisks in from: '%s' and to: '%s'", from, to)
		}
	}
	return nil
}

// processReplacement converts the given from and to strings into a regexp and
// a corresponding replacement string
func processReplacement(from, to string) (re *regexp.Regexp, replacer string, err error) {
	if !strings.Contains(from, "$") {
		// Convert asterisk wild cards to named variables
		from = replaceWildCards(from)
		to = replaceWildCards(to)
	}

	// Map from variable names to positions in the search regexp
	vars := make(map[string]int)

	var matcher string
	for len(from) > 0 {
		varName, rest, ok := peelLeadingVariable(from)
		if ok { // found variable
			if _, ok := vars[varName]; ok {
				// Cannot repeat the same variable in a 'from' string
				err = fmt.Errorf("Variable name '%s' used more than once", varName)
				return
			}
			// Put the variable in the map with its index in the string
			vars[varName] = len(vars) + 1
			matcher += "(.*?)"
			from = rest
			continue
		}

		c := rune(from[0])
		if c == '$' {
			err = fmt.Errorf("Extraneous '$'")
			return
		}
		if strings.ContainsRune(escapedChars, c) {
			// Add backslash before special chars
			matcher += `\`
		}
		matcher += string(c)
		from = from[1:]
	}
	matcher = fmt.Sprintf("^%s$", matcher)
	// The regexp we generated should always compile (it's not the user's fault)
	re = regexp.MustCompile(matcher)

	for len(to) > 0 {
		varName, rest, ok := peelLeadingVariable(to)
		if ok { // found variable
			if num, ok := vars[varName]; ok {
				replacer += fmt.Sprintf("${%d}", num)
				to = rest
			} else {
				err = fmt.Errorf("Unknown variable '%s'", varName)
				return
			}
			continue
		}

		c := rune(to[0])
		if c == '$' {
			err = fmt.Errorf("Extraneous '$'")
			return
		}
		replacer += string(c)
		to = to[1:]
	}
	return
}

// NewRenamer creates a Renamer that will use the given from and to slices to
// map namespaces
func NewRenamer(fromSlice, toSlice []string) (r *Renamer, err error) {
	if len(fromSlice) != len(toSlice) {
		err = fmt.Errorf("Different number of froms and tos")
		return
	}
	r = new(Renamer)
	for i := len(fromSlice) - 1; i >= 0; i-- {
		// reversed for replacement precedence
		from := fromSlice[i]
		to := toSlice[i]
		err = validateReplacement(from, to)
		if err != nil {
			return
		}
		matcher, replacer, e := processReplacement(from, to)
		if e != nil {
			err = fmt.Errorf("Invalid replacement from '%s' to '%s': %s", from, to, e)
			return
		}
		r.matchers = append(r.matchers, matcher)
		r.replacers = append(r.replacers, replacer)
	}
	return
}

// Get returns the rewritten namespace according to the renamer's rules
func (r *Renamer) Get(name string) string {
	for i, matcher := range r.matchers {
		if matcher.MatchString(name) {
			return matcher.ReplaceAllString(name, r.replacers[i])
		}
	}
	return name
}

// NewMatcher creates a matcher that will use the given list patterns to
// match namespaces
func NewMatcher(patterns []string) (m *Matcher, err error) {
	m = new(Matcher)
	for _, pattern := range patterns {
		if strings.Contains(pattern, "$") {
			err = fmt.Errorf("'$' is not allowed in include/exclude patternsj")
		}
		re, _, e := processReplacement(pattern, pattern)
		if e != nil {
			err = fmt.Errorf("%s processing include/exclude pattern: '%s'", err, pattern)
			return
		}
		m.matchers = append(m.matchers, re)
	}
	return
}

// Has returns whether the given namespace matches any of the matcher's patterns
func (m *Matcher) Has(name string) bool {
	for _, re := range m.matchers {
		if re.MatchString(name) {
			return true
		}
	}
	return false
}
