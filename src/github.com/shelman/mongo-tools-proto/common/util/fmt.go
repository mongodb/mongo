package util

import (
	"fmt"
	"time"
)

const (
	toolTimeFormat = "2006-01-02T15:04:05.000-0700"
)

// Panic with a formatted string
func Panicf(s string, args ...interface{}) {
	panic(fmt.Sprintf(s, args...))
}

// Println a formatted string
func Printlnf(s string, args ...interface{}) (int, error) {
	return fmt.Println(fmt.Sprintf(s, args...))
}

// PrintlnTimeStamped a formatted string along side the timestamp
func PrintlnTimeStamped(s string, args ...interface{}) (int, error) {
	return fmt.Println(time.Now().Format(toolTimeFormat),
		fmt.Sprintf(s, args...))
}

// PrintfTimeStamped a formatted string along side the timestamp
func PrintfTimeStamped(s string, args ...interface{}) (int, error) {
	line := fmt.Sprintf(s, args...)
	return fmt.Printf(fmt.Sprintf("%v %v", time.Now().Format(toolTimeFormat),
		line))
}
