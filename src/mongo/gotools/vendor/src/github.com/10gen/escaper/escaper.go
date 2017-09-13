/*
package escaper provides a utility for creating custom format strings.

By design, expanding a format string with this utility requires no
additional arguments (unlike fmt.Sprintf) by letting you easily register
your own escape handlers.

Basic usage:

    format := "%F{blue}text color%f, %K{red}background color%k, "
            + "%Bbold%b, %Uunderline%u, and %Sstandout%s."

    esc := escaper.Default()
    output := esc.Expand(format)

Advanced usage:

    format := "my name is %n, and the time is %D{3:04PM}"
    name := "Ben Bitdiddle"

    // use New() if you don't want the default ANSI escapes
    esc := escaper.New()
    esc.Register('n', func() string {
      return name
    })
    esc.RegisterArg('D', func(arg string) string {
      return time.Now().Format(arg)
    })
    output := esc.Expand(format)

*/
package escaper

import (
	"bytes"
	"fmt"
	"strings"
)

type matcher struct {
	takesArg bool

	fa func(string) string // used iff takesArg
	f  func() string       // (otherwise)
}

// Escaper maintains matchers for expanding format strings.
type Escaper struct {
	matchers map[rune]*matcher
}

// Expand performs the programmed escapes, returning the expanded string.
// The percent sign '%' is the escaping rune. For a literal pecent sign, use
// '%%'. If no escapes are assigned to a rune that is escaped, the literal
// rune is used.
//
// For escapes that take an argument, the argument is surrounded in curly
// braces '{<ARG>}' and immediately follows the escape rune. No expands are
// performed within the argument, and the '%' still escapes (for the sake of
// using a literal '}' via '%}'). If the escape rune is not immediately
// followed by a left curly brace, the argument is set to an empty string.
//
// Example:
//     esc := Default()
//     out := esc.Expand("80%% is %decent")
//     // out == "80% is decent"
//
//     out = esc.Expand("%Bthis%b is %F{cyan}colorful%f")
//     // out is "this is colorful", but with visual style
func (e *Escaper) Expand(in string) string {
	b := new(bytes.Buffer)
	var arg struct {
		on      bool
		esc     bool
		started bool // true iff arg-related runes have been read
		b       *bytes.Buffer
		m       *matcher
	}
	esc := false
	for _, r := range in {
		if arg.on {
			if !arg.started && r == '{' {
				arg.started = true
			}
			if arg.started {
				if arg.esc {
					arg.b.WriteRune(r)
					arg.esc = false
				} else if r == '%' {
					arg.esc = true
				} else if r != '}' {
					arg.b.WriteRune(r)
				} else {
					arg.on = false
					arg.b.ReadRune() // discard start brace
					s := arg.b.String()
					b.WriteString(arg.m.fa(s))
				}
				continue
			} else {
				// no arg = empty arg, read r like normal
				arg.on = false
				b.WriteString(arg.m.fa(""))
			}
		}
		if !esc {
			if r == '%' {
				esc = true
			} else {
				b.WriteRune(r)
			}
			continue
		}
		esc = false
		m, ok := e.matchers[r]
		if !ok {
			b.WriteRune(r)
			continue
		}
		if !m.takesArg {
			b.WriteString(m.f())
			continue
		}
		arg.on = true
		arg.started = false
		arg.b = new(bytes.Buffer)
		arg.m = m
	}
	// edge conditions after full string has been read
	if arg.on {
		s := arg.b.String()
		b.WriteString(arg.m.fa(s))
	}
	return b.String()
}

// Register adds an escape for a rune with its associated function.
// It overwrites previous matchers for the given rune.
//
// Example:
//     esc := New()
//     esc.Register('n', func() string {
//         return "I replace '%n'"
//     })
//     esc.Expand("%n") // "I replace '%n'"
func (e *Escaper) Register(r rune, f func() string) {
	e.matchers[r] = &matcher{false, nil, f}
}

// RegisterArg is like Register, but matches the escaped rune followed by an
// argument.
//
// Example:
//     esc := New()
//     esc.RegisterArg('q', func(arg string) string {
//         // takes a number, squares it
//         i, _ := strconv.Atoi(arg)
//         return strconv.Itoa(i * i)
//     })
//     esc.Expand("%q{20}") // "400"
func (e *Escaper) RegisterArg(r rune, fa func(string) string) {
	e.matchers[r] = &matcher{true, fa, nil}
}

// New returns a blank escaper, with no escapes preloaded.
func New() *Escaper {
	return &Escaper{map[rune]*matcher{}}
}

// Default returns a new escaper that has preloaded ANSI escapes
// (begin/end):
//     %F/%f    text color       (takes arg)
//     %K/%k    background color (takes arg)
//     %B/%b    bold
//     %U/%u    underline
//     %S/%s    standout
//
// The color escapes take an argument which can be a word or number as
// follows:
//     black   (0)
//     red     (1)
//     green   (2)
//     yellow  (3)
//     blue    (4)
//     magenta (5)
//     cyan    (6)
//     white   (7)
func Default() *Escaper {
	return &Escaper{
		matchers: map[rune]*matcher{
			'F': {true, colorF(30), nil},
			'f': {false, nil, codeF(39)},
			'K': {true, colorF(40), nil},
			'k': {false, nil, codeF(49)},
			'B': {false, nil, codeF(1)},
			'U': {false, nil, codeF(4)},
			'S': {false, nil, codeF(7)},
			'b': {false, nil, codeF(22)},
			'u': {false, nil, codeF(24)},
			's': {false, nil, codeF(27)},
		},
	}
}

func colorF(offset int) func(string) string {
	return func(arg string) string {
		var c int
		switch strings.ToLower(arg) {
		case "0":
			fallthrough
		case "black":
			c = 0
		case "1":
			fallthrough
		case "red":
			c = 1
		case "2":
			fallthrough
		case "green":
			c = 2
		case "3":
			fallthrough
		case "yellow":
			c = 3
		case "4":
			fallthrough
		case "blue":
			c = 4
		case "5":
			fallthrough
		case "magenta":
			c = 5
		case "6":
			fallthrough
		case "cyan":
			c = 6
		case "7":
			fallthrough
		case "white":
			c = 7
		default:
			c = 9
		}
		return code(offset + c)
	}
}

func code(c int) string {
	return fmt.Sprintf("\x1b[%dm", c)
}

func codeF(c int) func() string {
	return func() string {
		return code(c)
	}
}
