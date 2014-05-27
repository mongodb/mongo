package util

import (
	"fmt"
)

// Panic with a formatted string
func Panicf(s string, args ...interface{}) {
	panic(fmt.Sprintf(s, args...))
}

// Println a formatted string
func Printlnf(s string, args ...interface{}) (int, error) {
	return fmt.Println(fmt.Sprintf(s, args...))
}
