// Package common contains subpackages that are shared amongst the mongo
// tools.
package common

import (
	"strings"
)

// SplitNamespace returns the db and column from a single namespace string.
func SplitNamespace(ns string) (string, string) {
	i := strings.Index(ns, ".")
	if i != -1 {
		return ns[:i], ns[i+1:]
	}
	return "", ns
}
