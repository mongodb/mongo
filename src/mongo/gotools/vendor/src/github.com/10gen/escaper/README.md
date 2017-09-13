# escaper [![GoDoc](http://img.shields.io/badge/go-documentation-blue.svg?style=flat_square)](http://godoc.org/github.com/lucasem/escaper)

**`escaper`** lets you create your own formatting syntax. By design,
expanding a format string with this utility requires no additional arguments
(unlike fmt.Sprinf) by letting you easily register your own escape handlers.
This makes it **ideal for configurability**, where only a string is
necessary to specify a complex output format.

![escaper](http://i.imgur.com/qAAPq5y.png)

## Motivation

There is often desire to make some program output user-configurable.
A lot of software doesn't bother with configurability and has hard-coded
output formats. This package aims to eliminate this unfortunate paradigm,
and easily permit customizability where it is often disregarded. Inspired by
the style of zsh prompt expansion, **`escaper`** makes output formatting a
better abstraction for its users.

## Install

```bash
go get github.com/lucasem/escaper
```

## Examples

### Basics

```go
format := "add some ANSI %F{blue}text color%f easily"

esc := escaper.Default()
output := esc.Expand(format)
```

### Advanced

```go
format := "my name is %n, and the time is %D{3:04PM}"
name := "Ben Bitdiddle"

// use New() if you don't want the default ANSI escapes
esc := escaper.New()

// register a new escape
esc.Register('n', func() string {
  return name
})

// register an escape that takes an argument
esc.RegisterArg('D', func(arg string) string {
  return time.Now().Format(arg)
})

// "my name is Ben Bitdiddle, and the time is 11:15AM"
output := esc.Expand(format)
```

## The Default

The default escaper (`escaper.Default()`) supports the following ANSI
escapes:
- `%F{<color>}text%f` colors the text
- `%K{<color>}text%k` colors the background
- `%Btext%b` bolds the text
- `%Utext%u` underlines the text
- `%Stext%s` standouts (color inverts) the text

`<color>` can be one of:
```
black   (0)
red     (1)
green   (2)
yellow  (3)
blue    (4)
magenta (5)
cyan    (6)
white   (7)
```

## License

The MIT License (MIT) - see [`LICENSE`](https://github.com/lucasem/escaper/blob/master/LICENSE) for details
