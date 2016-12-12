// Copyright (c) 2013 The Go Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd.

package lint

import (
	"bytes"
	"flag"
	"go/parser"
	"go/printer"
	"go/token"
	"io/ioutil"
	"path"
	"regexp"
	"strings"
	"testing"
)

var lintMatch = flag.String("lint.match", "", "restrict testdata matches to this pattern")

func TestAll(t *testing.T) {
	l := new(Linter)
	rx, err := regexp.Compile(*lintMatch)
	if err != nil {
		t.Fatalf("Bad -lint.match value %q: %v", *lintMatch, err)
	}

	baseDir := "testdata"
	fis, err := ioutil.ReadDir(baseDir)
	if err != nil {
		t.Fatalf("ioutil.ReadDir: %v", err)
	}
	if len(fis) == 0 {
		t.Fatalf("no files in %v", baseDir)
	}
	for _, fi := range fis {
		if !rx.MatchString(fi.Name()) {
			continue
		}
		//t.Logf("Testing %s", fi.Name())
		src, err := ioutil.ReadFile(path.Join(baseDir, fi.Name()))
		if err != nil {
			t.Fatalf("Failed reading %s: %v", fi.Name(), err)
		}

		ins := parseInstructions(t, fi.Name(), src)
		if ins == nil {
			t.Errorf("Test file %v does not have instructions", fi.Name())
			continue
		}

		ps, err := l.Lint(fi.Name(), src)
		if err != nil {
			t.Errorf("Linting %s: %v", fi.Name(), err)
			continue
		}

		for _, in := range ins {
			ok := false
			for i, p := range ps {
				if p.Position.Line != in.Line {
					continue
				}
				if in.Match.MatchString(p.Text) {
					// remove this problem from ps
					copy(ps[i:], ps[i+1:])
					ps = ps[:len(ps)-1]

					//t.Logf("/%v/ matched at %s:%d", in.Match, fi.Name(), in.Line)
					ok = true
					break
				}
			}
			if !ok {
				t.Errorf("Lint failed at %s:%d; /%v/ did not match", fi.Name(), in.Line, in.Match)
			}
		}
		for _, p := range ps {
			t.Errorf("Unexpected problem at %s:%d: %v", fi.Name(), p.Position.Line, p.Text)
		}
	}
}

type instruction struct {
	Line  int            // the line number this applies to
	Match *regexp.Regexp // what pattern to match
}

// parseInstructions parses instructions from the comments in a Go source file.
// It returns nil if none were parsed.
func parseInstructions(t *testing.T, filename string, src []byte) []instruction {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, filename, src, parser.ParseComments)
	if err != nil {
		t.Fatalf("Test file %v does not parse: %v", filename, err)
	}
	var ins []instruction
	for _, cg := range f.Comments {
		ln := fset.Position(cg.Pos()).Line
		raw := cg.Text()
		for _, line := range strings.Split(raw, "\n") {
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			if line == "OK" && ins == nil {
				// so our return value will be non-nil
				ins = make([]instruction, 0)
				continue
			}
			if strings.Contains(line, "MATCH") {
				a, b := strings.Index(line, "/"), strings.LastIndex(line, "/")
				if a == -1 || a == b {
					t.Fatalf("Malformed match instruction %q at %v:%d", line, filename, ln)
				}
				pat := line[a+1 : b]
				rx, err := regexp.Compile(pat)
				if err != nil {
					t.Fatalf("Bad match pattern %q at %v:%d: %v", pat, filename, ln, err)
				}
				ins = append(ins, instruction{
					Line:  ln,
					Match: rx,
				})
			}
		}
	}
	return ins
}

func render(fset *token.FileSet, x interface{}) string {
	var buf bytes.Buffer
	if err := printer.Fprint(&buf, fset, x); err != nil {
		panic(err)
	}
	return buf.String()
}

func TestLine(t *testing.T) {
	tests := []struct {
		src    string
		offset int
		want   string
	}{
		{"single line file", 5, "single line file"},
		{"single line file with newline\n", 5, "single line file with newline\n"},
		{"first\nsecond\nthird\n", 2, "first\n"},
		{"first\nsecond\nthird\n", 9, "second\n"},
		{"first\nsecond\nthird\n", 14, "third\n"},
		{"first\nsecond\nthird with no newline", 16, "third with no newline"},
		{"first byte\n", 0, "first byte\n"},
	}
	for _, test := range tests {
		got := srcLine([]byte(test.src), token.Position{Offset: test.offset})
		if got != test.want {
			t.Errorf("srcLine(%q, offset=%d) = %q, want %q", test.src, test.offset, got, test.want)
		}
	}
}

func TestLintName(t *testing.T) {
	tests := []struct {
		name, want string
	}{
		{"foo_bar", "fooBar"},
		{"foo_bar_baz", "fooBarBaz"},
		{"Foo_bar", "FooBar"},
		{"foo_WiFi", "fooWiFi"},
		{"id", "id"},
		{"Id", "ID"},
		{"foo_id", "fooID"},
		{"fooId", "fooID"},
		{"fooUid", "fooUID"},
		{"idFoo", "idFoo"},
		{"uidFoo", "uidFoo"},
		{"midIdDle", "midIDDle"},
		{"APIProxy", "APIProxy"},
		{"ApiProxy", "APIProxy"},
		{"apiProxy", "apiProxy"},
		{"_Leading", "_Leading"},
		{"___Leading", "_Leading"},
		{"trailing_", "trailing"},
		{"trailing___", "trailing"},
		{"a_b", "aB"},
		{"a__b", "aB"},
		{"a___b", "aB"},
		{"Rpc1150", "RPC1150"},
	}
	for _, test := range tests {
		got := lintName(test.name)
		if got != test.want {
			t.Errorf("lintName(%q) = %q, want %q", test.name, got, test.want)
		}
	}
}
