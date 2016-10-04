// Copyright (c) 2013 The Go Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd.

// Package lint contains a linter for Go source code.
package lint

import (
	"bytes"
	"fmt"
	"go/ast"
	"go/parser"
	"go/printer"
	"go/token"
	"regexp"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

const styleGuideBase = "http://golang.org/s/comments"

// A Linter lints Go source code.
type Linter struct {
}

// Problem represents a problem in some source code.
type Problem struct {
	Position   token.Position // position in source file
	Text       string         // the prose that describes the problem
	Link       string         // (optional) the link to the style guide for the problem
	Confidence float64        // a value in (0,1] estimating the confidence in this problem's correctness
	LineText   string         // the source line
}

func (p *Problem) String() string {
	if p.Link != "" {
		return p.Text + "\n\n" + p.Link
	}
	return p.Text
}

// Lint lints src.
func (l *Linter) Lint(filename string, src []byte) ([]Problem, error) {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, "", src, parser.ParseComments)
	if err != nil {
		return nil, err
	}
	return (&file{fset: fset, f: f, src: src, filename: filename}).lint(), nil
}

// file represents a file being linted.
type file struct {
	fset     *token.FileSet
	f        *ast.File
	src      []byte
	filename string

	// sortable is the set of types in the file that implement sort.Interface.
	sortable map[string]bool
	// main is whether this file is in a "main" package.
	main bool

	problems []Problem
}

func (f *file) isTest() bool { return strings.HasSuffix(f.filename, "_test.go") }

func (f *file) lint() []Problem {
	f.scanSortable()
	f.main = f.isMain()

	f.lintPackageComment()
	f.lintImports()
	f.lintBlankImports()
	//f.lintExported()
	//f.lintNames()
	f.lintVarDecls()
	f.lintElses()
	f.lintRanges()
	f.lintErrorf()
	f.lintErrors()
	f.lintErrorStrings()
	//f.lintReceiverNames()
	f.lintIncDec()
	f.lintMake()

	return f.problems
}

func (f *file) errorf(n ast.Node, confidence float64, link, format string, a ...interface{}) {
	p := f.fset.Position(n.Pos())
	f.problems = append(f.problems, Problem{
		Position:   p,
		Text:       fmt.Sprintf(format, a...),
		Link:       link,
		Confidence: confidence,
		LineText:   srcLine(f.src, p),
	})
}

func (f *file) scanSortable() {
	f.sortable = make(map[string]bool)

	// bitfield for which methods exist on each type.
	const (
		Len = 1 << iota
		Less
		Swap
	)
	nmap := map[string]int{"Len": Len, "Less": Less, "Swap": Swap}
	has := make(map[string]int)
	f.walk(func(n ast.Node) bool {
		fn, ok := n.(*ast.FuncDecl)
		if !ok || fn.Recv == nil {
			return true
		}
		// TODO(dsymonds): We could check the signature to be more precise.
		recv := receiverType(fn)
		if i, ok := nmap[fn.Name.Name]; ok {
			has[recv] |= i
		}
		return false
	})
	for typ, ms := range has {
		if ms == Len|Less|Swap {
			f.sortable[typ] = true
		}
	}
}

func (f *file) isMain() bool {
	if f.f.Name.Name == "main" {
		return true
	}
	return false
}

// lintPackageComment checks package comments. It complains if
// there is no package comment, or if it is not of the right form.
// This has a notable false positive in that a package comment
// could rightfully appear in a different file of the same package,
// but that's not easy to fix since this linter is file-oriented.
func (f *file) lintPackageComment() {
	if f.isTest() {
		return
	}

	const link = styleGuideBase + "#Package_Comments"
	if f.f.Doc == nil {
		f.errorf(f.f, 0.2, link, "should have a package comment, unless it's in another file for this package")
		return
	}
	s := f.f.Doc.Text()
	prefix := "Package " + f.f.Name.Name + " "
	if ts := strings.TrimLeft(s, " \t"); ts != s {
		f.errorf(f.f.Doc, 1, link, "package comment should not have leading space")
		s = ts
	}
	// Only non-main packages need to keep to this form.
	if f.f.Name.Name != "main" && !strings.HasPrefix(s, prefix) {
		f.errorf(f.f.Doc, 1, link, `package comment should be of the form "%s..."`, prefix)
	}
}

// lintBlankImports complains if a non-main package has blank imports that are
// not documented.
func (f *file) lintBlankImports() {
	// In package main and in tests, we don't complain about blank imports.
	if f.main || f.isTest() {
		return
	}

	// The first element of each contiguous group of blank imports should have
	// an explanatory comment of some kind.
	for i, imp := range f.f.Imports {
		pos := f.fset.Position(imp.Pos())

		if !isBlank(imp.Name) {
			continue // Ignore non-blank imports.
		}
		if i > 0 {
			prev := f.f.Imports[i-1]
			prevPos := f.fset.Position(prev.Pos())
			if isBlank(prev.Name) && prevPos.Line+1 == pos.Line {
				continue // A subsequent blank in a group.
			}
		}

		// This is the first blank import of a group.
		if imp.Doc == nil && imp.Comment == nil {
			link := ""
			f.errorf(imp, 1, link, "a blank import should be only in a main or test package, or have a comment justifying it")
		}
	}
}

// lintImports examines import blocks.
func (f *file) lintImports() {

	for i, is := range f.f.Imports {
		_ = i
		if is.Name != nil && is.Name.Name == "." && !f.isTest() {
			f.errorf(is, 1, styleGuideBase+"#Import_Dot", "should not use dot imports")
		}

	}

}

const docCommentsLink = styleGuideBase + "#Doc_Comments"

// lintExported examines the doc comments of exported names.
// It complains if any required doc comments are missing,
// or if they are not of the right form. The exact rules are in
// lintFuncDoc, lintTypeDoc and lintValueSpecDoc; this function
// also tracks the GenDecl structure being traversed to permit
// doc comments for constants to be on top of the const block.
func (f *file) lintExported() {
	if f.isTest() {
		return
	}

	var lastGen *ast.GenDecl // last GenDecl entered.

	// Set of GenDecls that have already had missing comments flagged.
	genDeclMissingComments := make(map[*ast.GenDecl]bool)

	f.walk(func(node ast.Node) bool {
		switch v := node.(type) {
		case *ast.GenDecl:
			if v.Tok == token.IMPORT {
				return false
			}
			// token.CONST, token.TYPE or token.VAR
			lastGen = v
			return true
		case *ast.FuncDecl:
			f.lintFuncDoc(v)
			// Don't proceed inside funcs.
			return false
		case *ast.TypeSpec:
			// inside a GenDecl, which usually has the doc
			doc := v.Doc
			if doc == nil {
				doc = lastGen.Doc
			}
			f.lintTypeDoc(v, doc)
			// Don't proceed inside types.
			return false
		case *ast.ValueSpec:
			f.lintValueSpecDoc(v, lastGen, genDeclMissingComments)
			return false
		}
		return true
	})
}

var allCapsRE = regexp.MustCompile(`^[A-Z0-9_]+$`)

// lintNames examines all names in the file.
// It complains if any use underscores or incorrect known initialisms.
func (f *file) lintNames() {
	// Package names need slightly different handling than other names.
	if strings.Contains(f.f.Name.Name, "_") && !strings.HasSuffix(f.f.Name.Name, "_test") {
		f.errorf(f.f, 1, "http://golang.org/doc/effective_go.html#package-names", "don't use an underscore in package name")
	}

	check := func(id *ast.Ident, thing string) {
		if id.Name == "_" {
			return
		}

		// Handle two common styles from other languages that don't belong in Go.
		if len(id.Name) >= 5 && allCapsRE.MatchString(id.Name) && strings.Contains(id.Name, "_") {
			f.errorf(id, 0.8, styleGuideBase+"#Mixed_Caps", "don't use ALL_CAPS in Go names; use CamelCase")
			return
		}
		if len(id.Name) > 2 && id.Name[0] == 'k' && id.Name[1] >= 'A' && id.Name[1] <= 'Z' {
			should := string(id.Name[1]+'a'-'A') + id.Name[2:]
			f.errorf(id, 0.8, "", "don't use leading k in Go names; %s %s should be %s", thing, id.Name, should)
		}

		should := lintName(id.Name)
		if id.Name == should {
			return
		}
		if len(id.Name) > 2 && strings.Contains(id.Name[1:], "_") {
			f.errorf(id, 0.9, "http://golang.org/doc/effective_go.html#mixed-caps", "don't use underscores in Go names; %s %s should be %s", thing, id.Name, should)
			return
		}
		f.errorf(id, 0.8, styleGuideBase+"#Initialisms", "%s %s should be %s", thing, id.Name, should)
	}
	checkList := func(fl *ast.FieldList, thing string) {
		if fl == nil {
			return
		}
		for _, f := range fl.List {
			for _, id := range f.Names {
				check(id, thing)
			}
		}
	}
	f.walk(func(node ast.Node) bool {
		switch v := node.(type) {
		case *ast.AssignStmt:
			if v.Tok == token.ASSIGN {
				return true
			}
			for _, exp := range v.Lhs {
				if id, ok := exp.(*ast.Ident); ok {
					check(id, "var")
				}
			}
		case *ast.FuncDecl:
			if f.isTest() && (strings.HasPrefix(v.Name.Name, "Example") || strings.HasPrefix(v.Name.Name, "Test")) {
				return true
			}
			check(v.Name, "func")

			thing := "func"
			if v.Recv != nil {
				thing = "method"
			}
			checkList(v.Type.Params, thing+" parameter")
			checkList(v.Type.Results, thing+" result")
		case *ast.GenDecl:
			if v.Tok == token.IMPORT {
				return true
			}
			var thing string
			switch v.Tok {
			case token.CONST:
				thing = "const"
			case token.TYPE:
				thing = "type"
			case token.VAR:
				thing = "var"
			}
			for _, spec := range v.Specs {
				switch s := spec.(type) {
				case *ast.TypeSpec:
					check(s.Name, thing)
				case *ast.ValueSpec:
					for _, id := range s.Names {
						check(id, thing)
					}
				}
			}
		case *ast.InterfaceType:
			// Do not check interface method names.
			// They are often constrainted by the method names of concrete types.
			for _, x := range v.Methods.List {
				ft, ok := x.Type.(*ast.FuncType)
				if !ok { // might be an embedded interface name
					continue
				}
				checkList(ft.Params, "interface method parameter")
				checkList(ft.Results, "interface method result")
			}
		case *ast.RangeStmt:
			if v.Tok == token.ASSIGN {
				return true
			}
			if id, ok := v.Key.(*ast.Ident); ok {
				check(id, "range var")
			}
			if id, ok := v.Value.(*ast.Ident); ok {
				check(id, "range var")
			}
		case *ast.StructType:
			for _, f := range v.Fields.List {
				for _, id := range f.Names {
					check(id, "struct field")
				}
			}
		}
		return true
	})
}

// lintName returns a different name if it should be different.
func lintName(name string) (should string) {
	// Fast path for simple cases: "_" and all lowercase.
	if name == "_" {
		return name
	}
	allLower := true
	for _, r := range name {
		if !unicode.IsLower(r) {
			allLower = false
			break
		}
	}
	if allLower {
		return name
	}

	// Split camelCase at any lower->upper transition, and split on underscores.
	// Check each word for common initialisms.
	runes := []rune(name)
	w, i := 0, 0 // index of start of word, scan
	for i+1 <= len(runes) {
		eow := false // whether we hit the end of a word
		if i+1 == len(runes) {
			eow = true
		} else if runes[i+1] == '_' {
			// underscore; shift the remainder forward over any run of underscores
			eow = true
			n := 1
			for i+n+1 < len(runes) && runes[i+n+1] == '_' {
				n++
			}
			copy(runes[i+1:], runes[i+n+1:])
			runes = runes[:len(runes)-n]
		} else if unicode.IsLower(runes[i]) && !unicode.IsLower(runes[i+1]) {
			// lower->non-lower
			eow = true
		}
		i++
		if !eow {
			continue
		}

		// [w,i) is a word.
		word := string(runes[w:i])
		if u := strings.ToUpper(word); commonInitialisms[u] {
			// Keep consistent case, which is lowercase only at the start.
			if w == 0 && unicode.IsLower(runes[w]) {
				u = strings.ToLower(u)
			}
			// All the common initialisms are ASCII,
			// so we can replace the bytes exactly.
			copy(runes[w:], []rune(u))
		} else if w > 0 && strings.ToLower(word) == word {
			// already all lowercase, and not the first word, so uppercase the first character.
			runes[w] = unicode.ToUpper(runes[w])
		}
		w = i
	}
	return string(runes)
}

// commonInitialisms is a set of common initialisms.
// Only add entries that are highly unlikely to be non-initialisms.
// For instance, "ID" is fine (Freudian code is rare), but "AND" is not.
var commonInitialisms = map[string]bool{
	"API":   true,
	"ASCII": true,
	"CPU":   true,
	"CSS":   true,
	"DNS":   true,
	"EOF":   true,
	"HTML":  true,
	"HTTP":  true,
	"HTTPS": true,
	"ID":    true,
	"IP":    true,
	"JSON":  true,
	"LHS":   true,
	"QPS":   true,
	"RAM":   true,
	"RHS":   true,
	"RPC":   true,
	"SLA":   true,
	"SSH":   true,
	"TLS":   true,
	"TTL":   true,
	"UI":    true,
	"UID":   true,
	"URL":   true,
	"UTF8":  true,
	"VM":    true,
	"XML":   true,
}

// lintTypeDoc examines the doc comment on a type.
// It complains if they are missing from an exported type,
// or if they are not of the standard form.
func (f *file) lintTypeDoc(t *ast.TypeSpec, doc *ast.CommentGroup) {
	if !ast.IsExported(t.Name.Name) {
		return
	}
	if doc == nil {
		f.errorf(t, 1, docCommentsLink, "exported type %v should have comment or be unexported", t.Name)
		return
	}

	s := doc.Text()
	articles := [...]string{"A", "An", "The"}
	for _, a := range articles {
		if strings.HasPrefix(s, a+" ") {
			s = s[len(a)+1:]
			break
		}
	}
	if !strings.HasPrefix(s, t.Name.Name+" ") {
		f.errorf(doc, 1, docCommentsLink, `comment on exported type %v should be of the form "%v ..." (with optional leading article)`, t.Name, t.Name)
	}
}

var commonMethods = map[string]bool{
	"Error":     true,
	"Read":      true,
	"ServeHTTP": true,
	"String":    true,
	"Write":     true,
}

// lintFuncDoc examines doc comments on functions and methods.
// It complains if they are missing, or not of the right form.
// It has specific exclusions for well-known methods (see commonMethods above).
func (f *file) lintFuncDoc(fn *ast.FuncDecl) {
	if !ast.IsExported(fn.Name.Name) {
		// func is unexported
		return
	}
	kind := "function"
	name := fn.Name.Name
	if fn.Recv != nil {
		// method
		kind = "method"
		recv := receiverType(fn)
		if !ast.IsExported(recv) {
			// receiver is unexported
			return
		}
		if commonMethods[name] {
			return
		}
		switch name {
		case "Len", "Less", "Swap":
			if f.sortable[recv] {
				return
			}
		}
		name = recv + "." + name
	}
	if fn.Doc == nil {
		f.errorf(fn, 1, docCommentsLink, "exported %s %s should have comment or be unexported", kind, name)
		return
	}
	s := fn.Doc.Text()
	prefix := fn.Name.Name + " "
	if !strings.HasPrefix(s, prefix) {
		f.errorf(fn.Doc, 1, docCommentsLink, `comment on exported %s %s should be of the form "%s..."`, kind, name, prefix)
	}
}

// lintValueSpecDoc examines package-global variables and constants.
// It complains if they are not individually declared,
// or if they are not suitably documented in the right form (unless they are in a block that is commented).
func (f *file) lintValueSpecDoc(vs *ast.ValueSpec, gd *ast.GenDecl, genDeclMissingComments map[*ast.GenDecl]bool) {
	kind := "var"
	if gd.Tok == token.CONST {
		kind = "const"
	}

	if len(vs.Names) > 1 {
		// Check that none are exported except for the first.
		for _, n := range vs.Names[1:] {
			if ast.IsExported(n.Name) {
				f.errorf(vs, 1, "", "exported %s %s should have its own declaration", kind, n.Name)
				return
			}
		}
	}

	// Only one name.
	name := vs.Names[0].Name
	if !ast.IsExported(name) {
		return
	}

	if vs.Doc == nil {
		if gd.Doc == nil && !genDeclMissingComments[gd] {
			block := ""
			if kind == "const" && gd.Lparen.IsValid() {
				block = " (or a comment on this block)"
			}
			f.errorf(vs, 1, docCommentsLink, "exported %s %s should have comment%s or be unexported", kind, name, block)
			genDeclMissingComments[gd] = true
		}
		return
	}
	prefix := name + " "
	if !strings.HasPrefix(vs.Doc.Text(), prefix) {
		f.errorf(vs.Doc, 1, docCommentsLink, `comment on exported %s %s should be of the form "%s..."`, kind, name, prefix)
	}
}

// zeroLiteral is a set of ast.BasicLit values that are zero values.
// It is not exhaustive.
var zeroLiteral = map[string]bool{
	"false": true, // bool
	// runes
	`'\x00'`: true,
	`'\000'`: true,
	// strings
	`""`: true,
	"``": true,
	// numerics
	"0":   true,
	"0.":  true,
	"0.0": true,
	"0i":  true,
}

// lintVarDecls examines variable declarations. It complains about declarations with
// redundant LHS types that can be inferred from the RHS.
func (f *file) lintVarDecls() {
	var lastGen *ast.GenDecl // last GenDecl entered.

	f.walk(func(node ast.Node) bool {
		switch v := node.(type) {
		case *ast.GenDecl:
			if v.Tok != token.CONST && v.Tok != token.VAR {
				return false
			}
			lastGen = v
			return true
		case *ast.ValueSpec:
			if lastGen.Tok == token.CONST {
				return false
			}
			if len(v.Names) > 1 || v.Type == nil || len(v.Values) == 0 {
				return false
			}
			rhs := v.Values[0]
			// An underscore var appears in a common idiom for compile-time interface satisfaction,
			// as in "var _ Interface = (*Concrete)(nil)".
			if isIdent(v.Names[0], "_") {
				return false
			}
			// If the RHS is a zero value, suggest dropping it.
			zero := false
			if lit, ok := rhs.(*ast.BasicLit); ok {
				zero = zeroLiteral[lit.Value]
			} else if isIdent(rhs, "nil") {
				zero = true
			}
			if zero {
				f.errorf(rhs, 0.9, "", "should drop = %s from declaration of var %s; it is the zero value", f.render(rhs), v.Names[0])
				return false
			}
			// If the LHS type is an interface, don't warn, since it is probably a
			// concrete type on the RHS. Note that our feeble lexical check here
			// will only pick up interface{} and other literal interface types;
			// that covers most of the cases we care to exclude right now.
			// TODO(dsymonds): Use typechecker to make this heuristic more accurate.
			if _, ok := v.Type.(*ast.InterfaceType); ok {
				return false
			}
			// If the RHS is an untyped const, only warn if the LHS type is its default type.
			if defType, ok := isUntypedConst(rhs); ok && !isIdent(v.Type, defType) {
				return false
			}
			f.errorf(v.Type, 0.8, "", "should omit type %s from declaration of var %s; it will be inferred from the right-hand side", f.render(v.Type), v.Names[0])
			return false
		}
		return true
	})
}

// lintElses examines else blocks. It complains about any else block whose if block ends in a return.
func (f *file) lintElses() {
	// We don't want to flag if { } else if { } else { } constructions.
	// They will appear as an IfStmt whose Else field is also an IfStmt.
	// Record such a node so we ignore it when we visit it.
	ignore := make(map[*ast.IfStmt]bool)

	f.walk(func(node ast.Node) bool {
		ifStmt, ok := node.(*ast.IfStmt)
		if !ok || ifStmt.Else == nil {
			return true
		}
		if ignore[ifStmt] {
			return true
		}
		if elseif, ok := ifStmt.Else.(*ast.IfStmt); ok {
			ignore[elseif] = true
			return true
		}
		if _, ok := ifStmt.Else.(*ast.BlockStmt); !ok {
			// only care about elses without conditions
			return true
		}
		if len(ifStmt.Body.List) == 0 {
			return true
		}
		shortDecl := false // does the if statement have a ":=" initialization statement?
		if ifStmt.Init != nil {
			if as, ok := ifStmt.Init.(*ast.AssignStmt); ok && as.Tok == token.DEFINE {
				shortDecl = true
			}
		}
		lastStmt := ifStmt.Body.List[len(ifStmt.Body.List)-1]
		if _, ok := lastStmt.(*ast.ReturnStmt); ok {
			extra := ""
			if shortDecl {
				extra = " (move short variable declaration to its own line if necessary)"
			}
			f.errorf(ifStmt.Else, 1, styleGuideBase+"#Indent_Error_Flow", "if block ends with a return statement, so drop this else and outdent its block"+extra)
		}
		return true
	})
}

// lintRanges examines range clauses. It complains about redundant constructions.
func (f *file) lintRanges() {
	f.walk(func(node ast.Node) bool {
		rs, ok := node.(*ast.RangeStmt)
		if !ok {
			return true
		}
		if rs.Value == nil {
			// for x = range m { ... }
			return true // single var form
		}
		if !isIdent(rs.Value, "_") {
			// for ?, y = range m { ... }
			return true
		}

		f.errorf(rs.Value, 1, "", "should omit 2nd value from range; this loop is equivalent to `for %s %s range ...`", f.render(rs.Key), rs.Tok)
		return true
	})
}

// lintErrorf examines errors.New calls. It complains if its only argument is an fmt.Sprintf invocation.
func (f *file) lintErrorf() {
	f.walk(func(node ast.Node) bool {
		ce, ok := node.(*ast.CallExpr)
		if !ok {
			return true
		}
		if !isPkgDot(ce.Fun, "errors", "New") || len(ce.Args) != 1 {
			return true
		}
		arg := ce.Args[0]
		ce, ok = arg.(*ast.CallExpr)
		if !ok || !isPkgDot(ce.Fun, "fmt", "Sprintf") {
			return true
		}
		f.errorf(node, 1, "", "should replace errors.New(fmt.Sprintf(...)) with fmt.Errorf(...)")
		return true
	})
}

// lintErrors examines global error vars. It complains if they aren't named in the standard way.
func (f *file) lintErrors() {
	for _, decl := range f.f.Decls {
		gd, ok := decl.(*ast.GenDecl)
		if !ok || gd.Tok != token.VAR {
			continue
		}
		for _, spec := range gd.Specs {
			spec := spec.(*ast.ValueSpec)
			if len(spec.Names) != 1 || len(spec.Values) != 1 {
				continue
			}
			ce, ok := spec.Values[0].(*ast.CallExpr)
			if !ok {
				continue
			}
			if !isPkgDot(ce.Fun, "errors", "New") && !isPkgDot(ce.Fun, "fmt", "Errorf") {
				continue
			}

			id := spec.Names[0]
			prefix := "err"
			if id.IsExported() {
				prefix = "Err"
			}
			if !strings.HasPrefix(id.Name, prefix) {
				f.errorf(id, 0.9, "", "error var %s should have name of the form %sFoo", id.Name, prefix)
			}
		}
	}
}

func lintCapAndPunct(s string) (isCap, isPunct bool) {
	first, firstN := utf8.DecodeRuneInString(s)
	last, _ := utf8.DecodeLastRuneInString(s)
	isPunct = last == '.' || last == ':' || last == '!'
	isCap = unicode.IsUpper(first)
	if isCap && len(s) > firstN {
		// Don't flag strings starting with something that looks like an initialism.
		if second, _ := utf8.DecodeRuneInString(s[firstN:]); unicode.IsUpper(second) {
			isCap = false
		}
	}
	return
}

// lintErrorStrings examines error strings. It complains if they are capitalized or end in punctuation.
func (f *file) lintErrorStrings() {
	f.walk(func(node ast.Node) bool {
		ce, ok := node.(*ast.CallExpr)
		if !ok {
			return true
		}
		if !isPkgDot(ce.Fun, "errors", "New") && !isPkgDot(ce.Fun, "fmt", "Errorf") {
			return true
		}
		if len(ce.Args) < 1 {
			return true
		}
		str, ok := ce.Args[0].(*ast.BasicLit)
		if !ok || str.Kind != token.STRING {
			return true
		}
		s, _ := strconv.Unquote(str.Value) // can assume well-formed Go
		if s == "" {
			return true
		}
		isCap, isPunct := lintCapAndPunct(s)
		var msg string
		switch {
		case isCap && isPunct:
			msg = "error strings should not be capitalized and should not end with punctuation"
		case isCap:
			msg = "error strings should not be capitalized"
		case isPunct:
			msg = "error strings should not end with punctuation"
		default:
			return true
		}
		// People use proper nouns and exported Go identifiers in error strings,
		// so decrease the confidence of warnings for capitalization.
		conf := 0.8
		if isCap {
			conf = 0.6
		}
		f.errorf(str, conf, styleGuideBase+"#Error_Strings", msg)
		return true
	})
}

var badReceiverNames = map[string]bool{
	"me":   true,
	"this": true,
	"self": true,
}

// lintReceiverNames examines receiver names. It complains about inconsistent
// names used for the same type and names such as "this".
func (f *file) lintReceiverNames() {
	typeReceiver := map[string]string{}
	f.walk(func(n ast.Node) bool {
		fn, ok := n.(*ast.FuncDecl)
		if !ok || fn.Recv == nil {
			return true
		}
		names := fn.Recv.List[0].Names
		if len(names) < 1 {
			return true
		}
		name := names[0].Name
		const link = styleGuideBase + "#Receiver_Names"
		if badReceiverNames[name] {
			f.errorf(n, 1, link, `receiver name should be a reflection of its identity; don't use generic names such as "me", "this", or "self"`)
			return true
		}
		recv := receiverType(fn)
		if prev, ok := typeReceiver[recv]; ok && prev != name {
			f.errorf(n, 1, link, "receiver name %s should be consistent with previous receiver name %s for %s", name, prev, recv)
			return true
		}
		typeReceiver[recv] = name
		return true
	})
}

// lintIncDec examines statements that increment or decrement a variable.
// It complains if they don't use x++ or x--.
func (f *file) lintIncDec() {
	f.walk(func(n ast.Node) bool {
		as, ok := n.(*ast.AssignStmt)
		if !ok {
			return true
		}
		if len(as.Lhs) != 1 {
			return true
		}
		if !isOne(as.Rhs[0]) {
			return true
		}
		var suffix string
		switch as.Tok {
		case token.ADD_ASSIGN:
			suffix = "++"
		case token.SUB_ASSIGN:
			suffix = "--"
		default:
			return true
		}
		f.errorf(as, 0.8, "", "should replace %s with %s%s", f.render(as), f.render(as.Lhs[0]), suffix)
		return true
	})
}

// lineMake examines statements that declare and initialize a variable with make.
// It complains if they are constructing a zero element slice.
func (f *file) lintMake() {
	f.walk(func(n ast.Node) bool {
		as, ok := n.(*ast.AssignStmt)
		if !ok {
			return true
		}
		// Only want single var := assignment statements.
		if len(as.Lhs) != 1 || as.Tok != token.DEFINE {
			return true
		}
		ce, ok := as.Rhs[0].(*ast.CallExpr)
		if !ok {
			return true
		}
		// Check if ce is make([]T, 0).
		if !isIdent(ce.Fun, "make") || len(ce.Args) != 2 || !isZero(ce.Args[1]) {
			return true
		}
		at, ok := ce.Args[0].(*ast.ArrayType)
		if !ok || at.Len != nil {
			return true
		}
		f.errorf(as, 0.8, "", `can probably use "var %s %s" instead`, f.render(as.Lhs[0]), f.render(at))
		return true
	})
}

func receiverType(fn *ast.FuncDecl) string {
	switch e := fn.Recv.List[0].Type.(type) {
	case *ast.Ident:
		return e.Name
	case *ast.StarExpr:
		return e.X.(*ast.Ident).Name
	}
	panic(fmt.Sprintf("unknown method receiver AST node type %T", fn.Recv.List[0].Type))
}

func (f *file) walk(fn func(ast.Node) bool) {
	ast.Walk(walker(fn), f.f)
}

func (f *file) render(x interface{}) string {
	var buf bytes.Buffer
	if err := printer.Fprint(&buf, f.fset, x); err != nil {
		panic(err)
	}
	return buf.String()
}

func (f *file) debugRender(x interface{}) string {
	var buf bytes.Buffer
	if err := ast.Fprint(&buf, f.fset, x, nil); err != nil {
		panic(err)
	}
	return buf.String()
}

// walker adapts a function to satisfy the ast.Visitor interface.
// The function return whether the walk should proceed into the node's children.
type walker func(ast.Node) bool

func (w walker) Visit(node ast.Node) ast.Visitor {
	if w(node) {
		return w
	}
	return nil
}

func isIdent(expr ast.Expr, ident string) bool {
	id, ok := expr.(*ast.Ident)
	return ok && id.Name == ident
}

// isBlank returns whether id is the blank identifier "_".
// If id == nil, the answer is false.
func isBlank(id *ast.Ident) bool { return id != nil && id.Name == "_" }

func isPkgDot(expr ast.Expr, pkg, name string) bool {
	sel, ok := expr.(*ast.SelectorExpr)
	return ok && isIdent(sel.X, pkg) && isIdent(sel.Sel, name)
}

func isZero(expr ast.Expr) bool {
	lit, ok := expr.(*ast.BasicLit)
	return ok && lit.Kind == token.INT && lit.Value == "0"
}

func isOne(expr ast.Expr) bool {
	lit, ok := expr.(*ast.BasicLit)
	return ok && lit.Kind == token.INT && lit.Value == "1"
}

var basicLitKindTypes = map[token.Token]string{
	token.FLOAT:  "float64",
	token.IMAG:   "complex128",
	token.CHAR:   "rune",
	token.STRING: "string",
}

// isUntypedConst reports whether expr is an untyped constant,
// and indicates what its default type is.
func isUntypedConst(expr ast.Expr) (defType string, ok bool) {
	if isIntLiteral(expr) {
		return "int", true
	}
	if bl, ok := expr.(*ast.BasicLit); ok {
		if dt, ok := basicLitKindTypes[bl.Kind]; ok {
			return dt, true
		}
	}
	return "", false
}

func isIntLiteral(expr ast.Expr) bool {
	// Either a BasicLit with Kind token.INT,
	// or some combination of a UnaryExpr with Op token.SUB (for "-<lit>")
	// or a ParenExpr (for "(<lit>)").
Loop:
	for {
		switch v := expr.(type) {
		case *ast.UnaryExpr:
			if v.Op == token.SUB {
				expr = v.X
				continue Loop
			}
		case *ast.ParenExpr:
			expr = v.X
			continue Loop
		case *ast.BasicLit:
			if v.Kind == token.INT {
				return true
			}
		}
		return false
	}
}

// srcLine returns the complete line at p, including the terminating newline.
func srcLine(src []byte, p token.Position) string {
	// Run to end of line in both directions if not at line start/end.
	lo, hi := p.Offset, p.Offset+1
	for lo > 0 && src[lo-1] != '\n' {
		lo--
	}
	for hi < len(src) && src[hi-1] != '\n' {
		hi++
	}
	return string(src[lo:hi])
}
