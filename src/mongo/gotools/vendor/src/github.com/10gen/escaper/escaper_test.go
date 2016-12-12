package escaper

import (
	"strconv"
	"testing"
)

func TestUnassigned(t *testing.T) {
	esc := New()
	format := "I am 100%% sure this is a tes%t."
	expected := "I am 100% sure this is a test."
	output := esc.Expand(format)
	if output != expected {
		t.Errorf("expected %s, got %s", expected, output)
	} else {
		t.Logf("success: %s", output)
	}
}

func TestDefaultEscapes(t *testing.T) {
	esc := Default()

	// bold, underline, standout
	format := "%Bbold%bs, %Uunderline%us, and %Sstandout%ss."
	expected := "\x1b[1mbold\x1b[22ms, \x1b[4munderline\x1b[24ms, and \x1b[7mstandout\x1b[27ms."
	output := esc.Expand(format)
	if output != expected {
		t.Errorf("expected %s, got %s", expected, output)
	} else {
		t.Logf("success: %s", output)
	}

	// colors
	format = "%F{blue}color%f, %K{red}background%k and %F{6}with%f %K{4}numbers%k."
	expected = "\x1b[34mcolor\x1b[39m, \x1b[41mbackground\x1b[49m and \x1b[36mwith\x1b[39m \x1b[44mnumbers\x1b[49m."
	output = esc.Expand(format)
	if output != expected {
		t.Errorf("expected %s, got %s", expected, output)
		t.Fail()
	} else {
		t.Logf("success: %s", output)
	}
}

func TestCustomEscapes(t *testing.T) {
	format := "my name is %n, and my square is %q{20}."
	expected := "my name is Ben Bitdiddle, and my square is 400."
	name := "Ben Bitdiddle"
	esc := New()
	esc.Register('n', func() string {
		return name
	})
	esc.RegisterArg('q', func(arg string) string {
		i, _ := strconv.Atoi(arg)
		return strconv.Itoa(i * i)
	})
	output := esc.Expand(format)
	if output != expected {
		t.Errorf("expected %s, got %s", expected, output)
		t.Fail()
	} else {
		t.Logf("success: %s", output)
	}
}

func TestArgumentEscapes(t *testing.T) {
	format := "please echo %e{some %{100%%%} string}"
	expected := "please echo some {100%} string"
	esc := New()
	esc.RegisterArg('e', func(arg string) string {
		return arg
	})
	output := esc.Expand(format)
	if output != expected {
		t.Errorf("expected %s, got %s", expected, output)
		t.Fail()
	} else {
		t.Logf("success: %s", output)
	}
}

func TestEmptyArgument(t *testing.T) {
	format := "doubling a %d{string}, and an empty one: %d{},%dfoo"
	expected := "doubling a stringstring, and an empty one: ,foo"
	esc := New()
	esc.RegisterArg('d', func(arg string) string {
		return arg + arg
	})
	output := esc.Expand(format)
	if output != expected {
		t.Errorf("expected %s, got %s", expected, output)
		t.Fail()
	} else {
		t.Logf("success: %s", output)
	}
}

func TestEdge(t *testing.T) {
	format := "foo%a"
	expected := "foobar"
	esc := New()
	esc.RegisterArg('a', func(_ string) string {
		return "bar"
	})
	output := esc.Expand(format)
	if output != expected {
		t.Errorf("expected %s, got %s", expected, output)
		t.Fail()
	} else {
		t.Logf("success: %s", output)
	}
}
