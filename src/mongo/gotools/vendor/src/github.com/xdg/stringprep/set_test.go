package stringprep

import (
	"fmt"
	"testing"
)

func TestRanges(t *testing.T) {
	rangeTests := []struct {
		label string
		table Set
		in    rune
		out   bool
	}{
		// Table A.1
		{label: "A1", table: TableA1, in: 0x0221, out: true},
		{label: "A1", table: TableA1, in: 0x0955, out: true},
		{label: "A1", table: TableA1, in: 0x0956, out: true},
		{label: "A1", table: TableA1, in: 0x0957, out: true},
		{label: "A1", table: TableA1, in: 0x0020, out: false},
		// Table C.1.1
		{label: "C1.1", table: TableC1_1, in: 0x0020, out: true},
		{label: "C1.1", table: TableC1_1, in: 0x0040, out: false},
		// Table C.1.2
		{label: "C1.2", table: TableC1_2, in: 0x200A, out: true},
		{label: "C1.2", table: TableC1_2, in: 0x0040, out: false},
		// Table C.2.1
		{label: "C2.1", table: TableC2_1, in: 0x0000, out: true},
		{label: "C2.1", table: TableC2_1, in: 0x0010, out: true},
		{label: "C2.1", table: TableC2_1, in: 0x001F, out: true},
		{label: "C2.1", table: TableC2_1, in: 0x007F, out: true},
		{label: "C2.1", table: TableC2_1, in: 0x0040, out: false},
		// Table C.2.2
		{label: "C2.2", table: TableC2_2, in: 0x0080, out: true},
		{label: "C2.2", table: TableC2_2, in: 0x0090, out: true},
		{label: "C2.2", table: TableC2_2, in: 0x009F, out: true},
		{label: "C2.2", table: TableC2_2, in: 0x2028, out: true},
		{label: "C2.2", table: TableC2_2, in: 0xFEFF, out: true},
		{label: "C2.2", table: TableC2_2, in: 0x1D173, out: true},
		{label: "C2.2", table: TableC2_2, in: 0x1D17A, out: true},
		{label: "C2.2", table: TableC2_2, in: 0x0040, out: false},
		// Table C.3
		{label: "C3", table: TableC3, in: 0xE000, out: true},
		{label: "C3", table: TableC3, in: 0xF000, out: true},
		{label: "C3", table: TableC3, in: 0xF8FF, out: true},
		{label: "C3", table: TableC3, in: 0xF0000, out: true},
		{label: "C3", table: TableC3, in: 0xF1000, out: true},
		{label: "C3", table: TableC3, in: 0xFFFFD, out: true},
		{label: "C3", table: TableC3, in: 0x100000, out: true},
		{label: "C3", table: TableC3, in: 0x10ABCD, out: true},
		{label: "C3", table: TableC3, in: 0x10FFFD, out: true},
		{label: "C3", table: TableC3, in: 0x0040, out: false},
		// Table C.4
		{label: "C4", table: TableC4, in: 0xFDD0, out: true},
		{label: "C4", table: TableC4, in: 0xFFFF, out: true},
		{label: "C4", table: TableC4, in: 0x0040, out: false},
		// Table C.5
		{label: "C5", table: TableC5, in: 0xD801, out: true},
		{label: "C5", table: TableC5, in: 0x0040, out: false},
		// Table C.6
		{label: "C6", table: TableC6, in: 0xFFFA, out: true},
		{label: "C6", table: TableC6, in: 0x0040, out: false},
		// Table C.7
		{label: "C7", table: TableC7, in: 0x2FFB, out: true},
		{label: "C7", table: TableC7, in: 0x0040, out: false},
		// Table C.8
		{label: "C8", table: TableC8, in: 0x0341, out: true},
		{label: "C8", table: TableC8, in: 0x0040, out: false},
		// Table C.9
		{label: "C9", table: TableC9, in: 0xE0001, out: true},
		{label: "C9", table: TableC9, in: 0xE007E, out: true},
		{label: "C9", table: TableC9, in: 0x0040, out: false},
		// Table D.1
		{label: "D1", table: TableD1, in: 0x200F, out: true},
		{label: "D1", table: TableD1, in: 0x0040, out: false},
	}

	for _, c := range rangeTests {
		t.Run(fmt.Sprintf("%s 0x%04x", c.label, c.in), func(t *testing.T) {
			if got := c.table.Contains(c.in); got != c.out {
				t.Errorf("input '0x%04x' was %v, expected %v", c.in, got, c.out)
			}
		})
	}
}
