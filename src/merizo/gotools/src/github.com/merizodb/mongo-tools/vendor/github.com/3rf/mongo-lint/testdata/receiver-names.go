// Test for bad receiver names.

// Package foo ...
package foo

type foo struct{}

func (this foo) f1() { // MATCH /should be a reflection of its identity/
}

func (self foo) f2() { // MATCH /should be a reflection of its identity/
}

func (f foo) f3() {
}

func (foo) f4() {
}

type bar struct{}

func (b bar) f1() {
}

func (b bar) f2() {
}

func (a bar) f3() { // MATCH /receiver name a should be consistent with previous receiver name b for bar/
}

func (a *bar) f4() { // MATCH /receiver name a should be consistent with previous receiver name b for bar/
}

func (b *bar) f5() {
}

func (bar) f6() {
}
