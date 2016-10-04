// Test that we don't ask for comments on sort.Interface methods.

// Package pkg ...
package pkg

// T is ...
type T []int

// Len by itself should get documented.

func (t T) Len() int { return len(t) } // MATCH /exported method T\.Len.*should.*comment/

// U is ...
type U []int

func (u U) Len() int           { return len(u) }
func (u U) Less(i, j int) bool { return u[i] < u[j] }
func (u U) Swap(i, j int)      { u[i], u[j] = u[j], u[i] }

func (u U) Other() {} // MATCH /exported method U\.Other.*should.*comment/
