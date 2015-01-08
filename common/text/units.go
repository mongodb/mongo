package text

import (
	"fmt"
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
	return formatUnitAmount(binary, size, "%.1f %v", longByteUnits)
}

// FormatMegabyteAmount is equivalent to FormatByteAmount but expects
// an amount of MB instead of bytes.
func FormatMegabyteAmount(size int64) string {
	return formatUnitAmount(binary, size*1024*1024, "%.1f%v", shortByteUnits)
}

// FormatBitsWithLowPrecision takes in a bit (not byte) count and returns a
// formatted string including units with no decimal places
//  e.g. 12g, 0b, 124k
func FormatBits(size int64) string {
	return formatUnitAmount(decimal, size, "%.0f%v", shortBitUnits)
}

func formatUnitAmount(base, size int64, resultFormat string, units []string) string {
	result := float64(size)
	divisor := float64(base)
	var i int
	// keep dividing by base and incrementing our unit until
	// we hit the right unit or run out of unit strings
	for i = 1; result >= divisor && i < len(units); i++ {
		result /= divisor
	}
	return fmt.Sprintf(resultFormat, result, units[i-1])
}
