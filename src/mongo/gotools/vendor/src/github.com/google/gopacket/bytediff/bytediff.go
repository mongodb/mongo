// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// Package bytediff provides a simple diff utility for looking at differences in byte
// slices.  It's slow, clunky, and not particularly good by any measure, but
// it does provide very useful visualizations for diffs between small byte
// slices.
//
// Our diff algorithm uses a dynamic programming implementation of longest common
// substring to find matching parts of slices, then recursively calls itself on
// the prefix/suffix of that matching part for each packet.  This is a Bad Idea
// (tm) for normal (especially large) input, but for packets where large portions
// repeat frequently and we expect minor changes between results, it's actually
// quite useful.
package bytediff

import (
	"bytes"
	"fmt"
)

// OutputFormat tells a Differences.String call how to format the set of
// differences into a human-readable string.  Its internals are currently
// unexported because we may want to change them drastically in the future.  For
// the moment, please just use one of the provided OutputFormats that comes with
// this library.
type OutputFormat struct {
	start, finish, add, remove, change, reset string
}

var (
	// BashOutput uses bash escape sequences to color output.
	BashOutput = &OutputFormat{
		reset:  "\033[0m",
		remove: "\033[32m",
		add:    "\033[31m",
		change: "\033[33m",
	}
	// HTMLOutput uses a <pre> to wrap output, and <span>s to color it.
	// HTMLOutput is pretty experimental, so use at your own risk ;)
	HTMLOutput = &OutputFormat{
		start:  "<pre>",
		finish: "</pre>",
		reset:  "</span>",
		remove: "<span style='color:red'>",
		add:    "<span style='color:green'>",
		change: "<span style='color:yellow'>",
	}
)

// longestCommonSubstring uses a O(MN) dynamic programming approach to find the
// longest common substring in a set of slices.  It returns the index in each
// slice at which the substring begins, plus the length of the commonality.
func longestCommonSubstring(strA, strB []byte) (indexA, indexB, length int) {
	lenA, lenB := len(strA), len(strB)
	if lenA == 0 || lenB == 0 {
		return 0, 0, 0
	}
	arr := make([][]int, lenA)
	for i := 0; i < lenA; i++ {
		arr[i] = make([]int, lenB)
	}
	var maxLength int
	var maxA, maxB int
	for a := 0; a < lenA; a++ {
		for b := 0; b < lenB; b++ {
			if strA[a] == strB[b] {
				length := 1
				if a > 0 && b > 0 {
					length = arr[a-1][b-1] + 1
				}
				arr[a][b] = length
				if length > maxLength {
					maxLength = length
					maxA = a
					maxB = b
				}
			}
		}
	}
	a, b := maxA, maxB
	for a >= 0 && b >= 0 && strA[a] == strB[b] {
		indexA = a
		indexB = b
		a--
		b--
		length++
	}
	return
}

func intMax(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// Difference represents a single part of the data being diffed, containing
// information about both the original and new values.
// From and To are the sets of bytes in the original and the new byte slice.
//   !Replace        implies  From == To (no change)
//   len(To) == 0    implies  From is being deleted
//   len(From) == 0  implies  To is being inserted
//   else            implies  From is being replaced by To
type Difference struct {
	Replace  bool
	From, To []byte
}

// color returns the bash color for a given difference.
func (c *OutputFormat) color(d Difference) string {
	switch {
	case !d.Replace:
		return ""
	case len(d.From) == 0:
		return c.remove
	case len(d.To) == 0:
		return c.add
	default:
		return c.change
	}
}

// Diff diffs strA and strB, returning a list of differences which
// can be used to construct either the original or new string.
//
// Diff is optimized for comparing VERY SHORT slices.  It's meant for comparing
// things like packets off the wire, not large files or the like.
// As such, its runtime can be catastrophic if large inputs are passed in.
// You've been warned.
func Diff(strA, strB []byte) Differences {
	if len(strA) == 0 && len(strB) == 0 {
		return nil
	}
	ia, ib, l := longestCommonSubstring(strA, strB)
	if l == 0 {
		return Differences{
			Difference{true, strA, strB},
		}
	}
	beforeA, match, afterA := strA[:ia], strA[ia:ia+l], strA[ia+l:]
	beforeB, afterB := strB[:ib], strB[ib+l:]
	var diffs Differences
	diffs = append(diffs, Diff(beforeA, beforeB)...)
	diffs = append(diffs, Difference{false, match, match})
	diffs = append(diffs, Diff(afterA, afterB)...)
	return diffs
}

// Differences is a set of differences for a given diff'd pair of byte slices.
type Differences []Difference

// String outputs a previously diff'd set of strings, showing differences
// between them, highlighted by colors.
//
// The output format of this function is NOT guaranteed consistent, and may be
// changed at any time by the library authors.  It's meant solely for human
// consumption.
func (c *OutputFormat) String(diffs Differences) string {
	var buf bytes.Buffer
	count := 0
	fmt.Fprintf(&buf, "%s", c.start)
	fmt.Fprintf(&buf, "00000000 ")
	for i := 0; i < len(diffs); i++ {
		diff := diffs[i]
		color := c.color(diff)
		reset := ""
		if color != "" {
			reset = c.reset
		}
		fmt.Fprint(&buf, color)
		for _, b := range diff.From {
			fmt.Fprintf(&buf, " %02x", b)
			count++
			switch count % 16 {
			case 0:
				fmt.Fprintf(&buf, "%v\n%08x%v ", reset, count, color)
			case 8:
				fmt.Fprintf(&buf, " ")
			}
		}
		fmt.Fprint(&buf, reset)
	}
	fmt.Fprintf(&buf, "\n\n00000000 ")
	count = 0
	for i := 0; i < len(diffs); i++ {
		diff := diffs[i]
		str := diff.From
		if diff.Replace {
			str = diff.To
		}
		color := c.color(diff)
		reset := ""
		if color != "" {
			reset = c.reset
		}
		fmt.Fprint(&buf, color)
		for _, b := range str {
			fmt.Fprintf(&buf, " %02x", b)
			count++
			switch count % 16 {
			case 0:
				fmt.Fprintf(&buf, "%v\n%08x%v ", reset, count, color)
			case 8:
				fmt.Fprintf(&buf, " ")
			}
		}
		fmt.Fprint(&buf, reset)
	}
	fmt.Fprint(&buf, "\n")
	fmt.Fprintf(&buf, "%s", c.finish)
	return string(buf.Bytes())
}
