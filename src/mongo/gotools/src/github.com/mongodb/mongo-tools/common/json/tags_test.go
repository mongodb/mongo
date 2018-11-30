// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on github.com/golang/go by The Go Authors
// See THIRD-PARTY-NOTICES for original license terms.

package json

import (
	"testing"
)

func TestTagParsing(t *testing.T) {
	name, opts := parseTag("field,foobar,foo")
	if name != "field" {
		t.Fatalf("name = %q, want field", name)
	}
	for _, tt := range []struct {
		opt  string
		want bool
	}{
		{"foobar", true},
		{"foo", true},
		{"bar", false},
	} {
		if opts.Contains(tt.opt) != tt.want {
			t.Errorf("Contains(%q) = %v", tt.opt, !tt.want)
		}
	}
}
