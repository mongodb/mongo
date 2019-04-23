// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package text

import (
	"fmt"
	"math"
)

const (
	decimal = 1000
	binary  = 1024
)

var (
	longByteUnits  = []string{"B", "KB", "MB", "GB"}
	shortByteUnits = []string{"B", "K", "M", "G"}
	shortBitUnits  = []string{"b", "k", "m", "g"}
)

// FormatByteAmount takes an int64 representing a size in bytes and
// returns a formatted string of a minimum amount of significant figures.
//  e.g. 12.4 GB, 0.0 B, 124.5 KB
func FormatByteAmount(size int64) string {
	return formatUnitAmount(binary, size, 3, longByteUnits)
}

// FormatMegabyteAmount is equivalent to FormatByteAmount but expects
// an amount of MB instead of bytes.
func FormatMegabyteAmount(size int64) string {
	return formatUnitAmount(binary, size*1024*1024, 3, shortByteUnits)
}

// FormatBits takes in a bit (not byte) count and returns a formatted string
// including units with three total digits (except if it is less than 1k)
// e.g. 12.0g, 0b, 124k
func FormatBits(size int64) string {
	return formatUnitAmount(decimal, size, 3, shortBitUnits)
}

// formatUnitAmount formats the size using the units and at least minDigits
// numbers, unless the number is already less than the base, where no decimal
// will be added
func formatUnitAmount(base, size int64, minDigits int, units []string) string {
	result := float64(size)
	divisor := float64(base)
	var shifts int
	// keep dividing by base and incrementing our unit until
	// we hit the right unit or run out of unit strings
	for ; result >= divisor && shifts < len(units)-1; shifts++ {
		result /= divisor
	}
	result = round(result, minDigits)

	var precision int                  // Number of digits to show after the decimal
	len := 1 + int(math.Log10(result)) // Number of pre-decimal digits in result
	if shifts != 0 && len < minDigits {
		// Add as many decimal digits as we can
		precision = minDigits - len
	}
	format := fmt.Sprintf("%%.%df%%s", precision)
	return fmt.Sprintf(format, result, units[shifts])
}

// round applies the gradeschool method to round to the nth place
func round(result float64, precision int) float64 {
	divisor := float64(math.Pow(10.0, float64(precision-1)))
	// round(x) == floor(x + 0.5)
	return math.Floor(result*divisor+0.5) / divisor
}
