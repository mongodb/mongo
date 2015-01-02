package text

import (
	"fmt"
)

// FormatByteAmount takes an int64 representing a size in bytes and
// returns a formatted string of a minimum amount of significant figures.
func FormatByteAmount(size int64) string {
	result := float64(size) / 1024
	unit := "KB"
	if result >= 1024 {
		unit = "MB"
		result = result / 1024
	}
	if result >= 1024 {
		unit = "GB"
		result = result / 1024
	}
	return fmt.Sprintf("%.1f %v", result, unit)
}
