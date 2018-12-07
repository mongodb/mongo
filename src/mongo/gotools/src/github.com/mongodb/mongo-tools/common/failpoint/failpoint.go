// +build failpoints

// Package failpoint implements triggers for custom debugging behavoir
package failpoint

import (
	"strings"
)

var values map[string]string

func init() {
	values = make(map[string]string)
}

// ParseFailpoints registers a comma-separated list of failpoint=value pairs
func ParseFailpoints(arg string) {
	args := strings.Split(arg, ",")
	for _, fp := range args {
		if sep := strings.Index(fp, "="); sep != -1 {
			key := fp[:sep]
			val := fp[sep+1:]
			values[key] = val
			continue
		}
		values[fp] = ""
	}
}

// Get returns the value of the given failpoint and true, if it exists, and
// false otherwise
func Get(fp string) (string, bool) {
	val, ok := values[fp]
	return val, ok
}

// Enabled returns true iff the given failpoint has been turned on
func Enabled(fp string) bool {
	_, ok := Get(fp)
	return ok
}
