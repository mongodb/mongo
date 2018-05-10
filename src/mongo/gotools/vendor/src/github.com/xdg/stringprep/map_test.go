package stringprep

import (
	"fmt"
	"reflect"
	"testing"
)

func TestMapping(t *testing.T) {
	mappingTests := []struct {
		label  string
		table  Mapping
		in     rune
		exists bool
		out    []rune
	}{
		// Table B1
		{label: "B1", table: TableB1, in: 0x00AD, exists: true, out: []rune{}},
		{label: "B1", table: TableB1, in: 0x0040, exists: false, out: nil},
		// Table B2
		{label: "B2", table: TableB2, in: 0x0043, exists: true, out: []rune{0x0063}},
		{label: "B2", table: TableB2, in: 0x00DF, exists: true, out: []rune{0x0073, 0x0073}},
		{label: "B2", table: TableB2, in: 0x1F56, exists: true, out: []rune{0x03C5, 0x0313, 0x0342}},
		{label: "B2", table: TableB2, in: 0x0040, exists: false, out: nil},
		// Table B3
		{label: "B3", table: TableB3, in: 0x1FF7, exists: true, out: []rune{0x03C9, 0x0342, 0x03B9}},
		{label: "B3", table: TableB3, in: 0x0040, exists: false, out: nil},
	}

	for _, c := range mappingTests {
		t.Run(fmt.Sprintf("%s 0x%04x", c.label, c.in), func(t *testing.T) {
			got, ok := c.table.Map(c.in)
			switch c.exists {
			case true:
				if !ok {
					t.Errorf("input '0x%04x' was not found, but should have been", c.in)
				}
				if !reflect.DeepEqual(got, c.out) {
					t.Errorf("input '0x%04x' was %v, expected %v", c.in, got, c.out)
				}
			case false:
				if ok {
					t.Errorf("input '0x%04x' was found, but should not have been", c.in)
				}
				if got != nil {
					t.Errorf("input '0x%04x' was %v, expected %v", c.in, got, c.out)
				}
			}
		})
	}
}
