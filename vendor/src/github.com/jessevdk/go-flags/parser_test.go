package flags

import (
	"reflect"
	"testing"
	"time"
)

type defaultOptions struct {
	Int        int `long:"i"`
	IntDefault int `long:"id" default:"1"`

	Time        time.Duration `long:"t"`
	TimeDefault time.Duration `long:"td" default:"1m"`

	Map        map[string]int `long:"m"`
	MapDefault map[string]int `long:"md" default:"a:1"`

	Slice        []int `long:"s"`
	SliceDefault []int `long:"sd" default:"1" default:"2"`
}

func TestDefaults(t *testing.T) {
	var tests = []struct {
		msg      string
		args     []string
		expected defaultOptions
	}{
		{
			msg:  "no arguments, expecting default values",
			args: []string{},
			expected: defaultOptions{
				Int:        0,
				IntDefault: 1,

				Time:        0,
				TimeDefault: time.Minute,

				Map:        map[string]int{},
				MapDefault: map[string]int{"a": 1},

				Slice:        []int{},
				SliceDefault: []int{1, 2},
			},
		},
		{
			msg:  "non-zero value arguments, expecting overwritten arguments",
			args: []string{"--i=3", "--id=3", "--t=3ms", "--td=3ms", "--m=c:3", "--md=c:3", "--s=3", "--sd=3"},
			expected: defaultOptions{
				Int:        3,
				IntDefault: 3,

				Time:        3 * time.Millisecond,
				TimeDefault: 3 * time.Millisecond,

				Map:        map[string]int{"c": 3},
				MapDefault: map[string]int{"c": 3},

				Slice:        []int{3},
				SliceDefault: []int{3},
			},
		},
		{
			msg:  "zero value arguments, expecting overwritten arguments",
			args: []string{"--i=0", "--id=0", "--t=0ms", "--td=0s", "--m=:0", "--md=:0", "--s=0", "--sd=0"},
			expected: defaultOptions{
				Int:        0,
				IntDefault: 0,

				Time:        0,
				TimeDefault: 0,

				Map:        map[string]int{"": 0},
				MapDefault: map[string]int{"": 0},

				Slice:        []int{0},
				SliceDefault: []int{0},
			},
		},
	}

	for _, test := range tests {
		var opts defaultOptions

		_, err := ParseArgs(&opts, test.args)
		if err != nil {
			t.Fatalf("%s:\nUnexpected error: %v", test.msg, err)
		}

		if opts.Slice == nil {
			opts.Slice = []int{}
		}

		if !reflect.DeepEqual(opts, test.expected) {
			t.Errorf("%s:\nUnexpected options with arguments %+v\nexpected\n%+v\nbut got\n%+v\n", test.msg, test.args, test.expected, opts)
		}
	}
}
