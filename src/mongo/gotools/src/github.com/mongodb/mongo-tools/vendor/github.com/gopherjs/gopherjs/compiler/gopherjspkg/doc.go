// Package gopherjspkg provides core GopherJS packages via a virtual filesystem.
//
// Core GopherJS packages are packages that are critical for GopherJS compiler
// operation. They are needed to build the Go standard library with GopherJS.
// Currently, they include:
//
// 	github.com/gopherjs/gopherjs/js
// 	github.com/gopherjs/gopherjs/nosync
//
package gopherjspkg

//go:generate vfsgendev -source="github.com/gopherjs/gopherjs/compiler/gopherjspkg".FS -tag=gopherjsdev
